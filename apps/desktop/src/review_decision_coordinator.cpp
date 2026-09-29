#include "review_decision_coordinator.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage decision_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

std::optional<BackendReviewDecisionFlag> review_decision_flag_from_name(
    const QString& flag
) {
    if (flag == QStringLiteral("unflagged")) {
        return BackendReviewDecisionFlag::Unflagged;
    }
    if (flag == QStringLiteral("picked")) {
        return BackendReviewDecisionFlag::Picked;
    }
    if (flag == QStringLiteral("rejected")) {
        return BackendReviewDecisionFlag::Rejected;
    }
    return std::nullopt;
}

QString review_decision_flag_name(const BackendReviewDecisionFlag flag) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return QStringLiteral("unflagged");
    case BackendReviewDecisionFlag::Picked:
        return QStringLiteral("picked");
    case BackendReviewDecisionFlag::Rejected:
        return QStringLiteral("rejected");
    }
    throw std::invalid_argument("unknown Review decision flag");
}

ReviewDecisionCoordinator::ReviewDecisionCoordinator(
    Operations operations,
    CurrentStateResolver current_state_resolver,
    StateApplied state_applied,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)),
      current_state_resolver_(std::move(current_state_resolver)),
      state_applied_(std::move(state_applied)) {
    if (!operations_.mutate || !operations_.authoritative_state) {
        throw std::invalid_argument(
            "complete Review decision operations are required"
        );
    }
    if (!current_state_resolver_ || !state_applied_) {
        throw std::invalid_argument(
            "Review decision state resolver and projection are required"
        );
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewDecisionCoordinator::finishTask
    );
}

ReviewDecisionCoordinator::~ReviewDecisionCoordinator() {
    watcher_.waitForFinished();
}

bool ReviewDecisionCoordinator::busy() const noexcept {
    return session_.busy();
}

bool ReviewDecisionCoordinator::canUndo() const {
    return session_.canUndo();
}

const LocalizedUiMessage& ReviewDecisionCoordinator::statusMessage() const noexcept {
    return status_message_;
}

QString ReviewDecisionCoordinator::statusText() const {
    return status_message_.translated();
}

bool ReviewDecisionCoordinator::setFlag(
    const QString& photo_id,
    const QString& flag,
    const bool admitted
) {
    const auto desired_flag = review_decision_flag_from_name(flag);
    if (!desired_flag) {
        setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Unsupported Review flag"
        )));
        return false;
    }
    if (!admitted || session_.busy()) {
        return false;
    }
    const auto current = currentState(
        photo_id,
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Select a loaded photo before setting a flag"
        ),
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Could not read the current flag decision · %1"
        )
    );
    if (!current) {
        return false;
    }
    const auto request = session_.beginSet(
        *current,
        *desired_flag,
        current->rating
    );
    if (!request) {
        setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Flag already matches the selected photo"
        )));
        emit decisionCommitted(photo_id);
        return false;
    }
    setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Appending an explicit flag decision…"
    )));
    startMutation(*request);
    return true;
}

bool ReviewDecisionCoordinator::setRating(
    const QString& photo_id,
    const int rating,
    const bool admitted
) {
    if (rating < 0 || rating > 5) {
        setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Rating must be between 0 and 5 stars"
        )));
        return false;
    }
    if (!admitted || session_.busy()) {
        return false;
    }
    const auto current = currentState(
        photo_id,
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Select a loaded photo before setting a rating"
        ),
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Could not read the current star rating · %1"
        )
    );
    if (!current) {
        return false;
    }
    const auto request = session_.beginSet(
        *current,
        current->flag,
        static_cast<std::uint8_t>(rating)
    );
    if (!request) {
        setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Rating already matches the selected photo"
        )));
        emit decisionCommitted(photo_id);
        return false;
    }
    setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Appending an explicit star rating…"
    )));
    startMutation(*request);
    return true;
}

bool ReviewDecisionCoordinator::undo(const bool admitted) {
    if (!admitted || session_.busy()) {
        return false;
    }
    const auto request = session_.beginUndo();
    if (!request) {
        setStatusMessage(decision_message(
            session_.undoDepth() > 0
                ? QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "Local undo is disabled because the authoritative decision changed"
                  )
                : QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "No decision from this app session is available to undo"
                  )
        ));
        emit stateChanged();
        return false;
    }
    setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Appending an inverse decision event…"
    )));
    startMutation(*request);
    return true;
}

void ReviewDecisionCoordinator::reconcile(
    BackendReviewDecisionState authoritative
) {
    session_.reconcile(std::move(authoritative));
}

void ReviewDecisionCoordinator::retranslateUi() {
    emit statusTextChanged();
}

ReviewDecisionCoordinator::TaskResult ReviewDecisionCoordinator::runMutation(
    Operations operations,
    ReviewDecisionMutationRequest request
) {
    TaskResult result;
    result.is_undo = request.is_undo;
    try {
        result.receipt = operations.mutate(
            request.photo_id,
            request.expected_head_sequence,
            request.desired_flag,
            request.desired_rating
        );
        return result;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    try {
        result.authoritative =
            operations.authoritative_state(request.photo_id);
        result.has_authoritative = true;
    } catch (const std::exception& error) {
        result.refresh_error = QString::fromUtf8(error.what());
    }
    return result;
}

std::optional<BackendReviewDecisionState>
ReviewDecisionCoordinator::currentState(
    const QString& photo_id,
    const char* const missing_message,
    const char* const invalid_message
) {
    try {
        const auto current = current_state_resolver_(photo_id);
        if (current) {
            return current;
        }
    } catch (const std::exception& error) {
        setStatusMessage(decision_message(
            invalid_message,
            {QString::fromUtf8(error.what())}
        ));
        return std::nullopt;
    }
    setStatusMessage(decision_message(missing_message));
    return std::nullopt;
}

void ReviewDecisionCoordinator::startMutation(
    const ReviewDecisionMutationRequest& request
) {
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run(
        runMutation,
        operations_,
        request
    ));
}

void ReviewDecisionCoordinator::finishTask() {
    const TaskResult result = watcher_.result();
    if (!result.error.isEmpty()) {
        session_.fail();
        if (result.has_authoritative) {
            applyState(result.authoritative);
        }
        emit stateChanged();
        if (result.is_undo && result.has_authoritative
            && !session_.canUndo()) {
            setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
                "ReviewController",
                "Undo blocked · authoritative decision changed outside this session"
            )));
        } else if (result.is_undo && session_.canUndo()) {
            setStatusMessage(decision_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Undo write failed · unchanged state remains retryable · %1"
                ),
                {result.error}
            ));
        } else if (result.has_authoritative) {
            setStatusMessage(decision_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Decision write failed · authoritative state refreshed · %1"
                ),
                {result.error}
            ));
        } else {
            setStatusMessage(decision_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Decision write failed · refresh also failed · %1 · %2"
                ),
                {result.error, result.refresh_error}
            ));
        }
        return;
    }

    if (!session_.complete(result.receipt)) {
        emit stateChanged();
        setStatusMessage(decision_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Decision receipt was invalid; local state retained"
        )));
        return;
    }
    applyState(result.receipt.after);
    emit stateChanged();
    if (result.is_undo) {
        setStatusMessage(decision_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Inverse decision appended · sequence %1 · history retained"
            ),
            {result.receipt.sequence}
        ));
        emit undone();
    } else {
        setStatusMessage(decision_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Manual decision recorded · sequence %1"
            ),
            {result.receipt.sequence}
        ));
        emit decisionCommitted(result.receipt.after.photo_id);
    }
}

void ReviewDecisionCoordinator::applyState(
    const BackendReviewDecisionState& state
) {
    session_.reconcile(state);
    state_applied_(state);
    emit decisionApplied(
        state.photo_id,
        state.head_sequence,
        review_decision_flag_name(state.flag),
        static_cast<int>(state.rating)
    );
}

void ReviewDecisionCoordinator::setStatusMessage(LocalizedUiMessage status) {
    if (status_message_ == status) {
        return;
    }
    status_message_ = std::move(status);
    emit statusTextChanged();
}

#include "review_comparison_coordinator.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage comparison_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewComparisonCoordinator::ReviewComparisonCoordinator(
    Operations operations,
    VisualSourceResolver visual_source_resolver,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)),
      visual_source_resolver_(std::move(visual_source_resolver)) {
    if (!operations_.prepare || !operations_.confirm_ready
        || !operations_.cancel || !operations_.record || !operations_.forget) {
        throw std::invalid_argument(
            "complete Review comparison operations are required"
        );
    }
    if (!visual_source_resolver_) {
        throw std::invalid_argument(
            "Review comparison visual source resolver is required"
        );
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewComparisonCoordinator::finishTask
    );
}

ReviewComparisonCoordinator::~ReviewComparisonCoordinator() {
    watcher_.waitForFinished();
}

bool ReviewComparisonCoordinator::busy() const noexcept {
    return session_.busy();
}

bool ReviewComparisonCoordinator::canForget() const noexcept {
    return session_.canForget();
}

int ReviewComparisonCoordinator::activeCount() const noexcept {
    return session_.activeCount();
}

QString ReviewComparisonCoordinator::statusText() const {
    return status_message_.translated();
}

QVariantMap ReviewComparisonCoordinator::prepare(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) {
    if (session_.busy()) {
        return {};
    }
    if (left_visual_handle.trimmed().isEmpty()
        || right_visual_handle.trimmed().isEmpty()) {
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Choose two verified visuals before comparing"
        )));
        return {};
    }
    try {
        const BackendReviewComparisonPresentation presentation =
            operations_.prepare(left_visual_handle, right_visual_handle);
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Loading two exact Compare frames with durable provenance…"
        )));
        return {
            {QStringLiteral("presentationId"), presentation.presentation_id},
            {
                QStringLiteral("leftRequestTicket"),
                presentation.left_request_ticket,
            },
            {
                QStringLiteral("rightRequestTicket"),
                presentation.right_request_ticket,
            },
            {
                QStringLiteral("leftSource"),
                visual_source_resolver_(presentation.left_request_ticket),
            },
            {
                QStringLiteral("rightSource"),
                visual_source_resolver_(presentation.right_request_ticket),
            },
        };
    } catch (const std::exception& error) {
        setStatusMessage(comparison_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Cannot prepare exact comparison · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
        return {};
    }
}

bool ReviewComparisonCoordinator::confirmReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) {
    if (presentation_id.trimmed().isEmpty()
        || left_request_ticket.trimmed().isEmpty()
        || right_request_ticket.trimmed().isEmpty()) {
        return false;
    }
    try {
        operations_.confirm_ready(
            presentation_id,
            left_request_ticket,
            right_request_ticket
        );
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Exact encoded artifacts and decoded Compare frames verified"
        )));
        return true;
    } catch (const std::exception& error) {
        setStatusMessage(comparison_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Comparison frame verification failed · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
        return false;
    }
}

void ReviewComparisonCoordinator::cancel(const QString& presentation_id) {
    if (presentation_id.trimmed().isEmpty() || session_.busy()) {
        return;
    }
    try {
        operations_.cancel(presentation_id);
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Comparison presentation closed"
        )));
    } catch (const std::exception& error) {
        setStatusMessage(comparison_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Cannot close comparison presentation · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
    }
}

bool ReviewComparisonCoordinator::record(
    const QString& presentation_id,
    const int outcome,
    const bool admitted
) {
    const auto resolved_outcome = pairwiseOutcome(outcome);
    if (!resolved_outcome) {
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Comparison outcome is not supported"
        )));
        return false;
    }
    if (presentation_id.trimmed().isEmpty()) {
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Prepare and verify the comparison first"
        )));
        return false;
    }
    if (!admitted) {
        return false;
    }
    if (!session_.beginRecord()) {
        return false;
    }
    emit stateChanged();
    setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Recording append-only comparison evidence…"
    )));
    watcher_.setFuture(QtConcurrent::run(
        runRecord,
        operations_.record,
        presentation_id,
        *resolved_outcome
    ));
    return true;
}

bool ReviewComparisonCoordinator::forgetLast() {
    const auto event_id = session_.beginForget();
    if (!event_id) {
        return false;
    }
    emit stateChanged();
    setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Appending a forget fact for the latest evidence…"
    )));
    watcher_.setFuture(QtConcurrent::run(
        runForget,
        operations_.forget,
        *event_id
    ));
    return true;
}

void ReviewComparisonCoordinator::retranslateUi() {
    emit statusTextChanged();
}

std::optional<BackendPairwiseOutcome>
ReviewComparisonCoordinator::pairwiseOutcome(const int outcome) {
    switch (outcome) {
    case 0:
        return BackendPairwiseOutcome::LeftPreferred;
    case 1:
        return BackendPairwiseOutcome::RightPreferred;
    case 2:
        return BackendPairwiseOutcome::KeepBoth;
    case 3:
        return BackendPairwiseOutcome::KeepNeither;
    case 4:
        return BackendPairwiseOutcome::CannotCompare;
    default:
        return std::nullopt;
    }
}

ReviewComparisonCoordinator::TaskResult
ReviewComparisonCoordinator::runRecord(
    std::function<BackendFeedbackReceipt(
        const QString& presentation_id,
        BackendPairwiseOutcome outcome
    )> operation,
    QString presentation_id,
    const BackendPairwiseOutcome outcome
) {
    TaskResult result;
    result.kind = TaskKind::Record;
    try {
        result.feedback = operation(presentation_id, outcome);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewComparisonCoordinator::TaskResult
ReviewComparisonCoordinator::runForget(
    std::function<BackendForgetReceipt(const QString& event_id)> operation,
    QString event_id
) {
    TaskResult result;
    result.kind = TaskKind::Forget;
    try {
        result.forget = operation(event_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewComparisonCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    if (!result.error.isEmpty()) {
        session_.fail();
        emit stateChanged();
        setStatusMessage(comparison_message(
            result.kind == TaskKind::Record
                ? QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "Evidence write failed · pair retained · %1"
                  )
                : QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "Forget write failed · evidence retained · %1"
                  ),
            {result.error}
        ));
        return;
    }

    if (result.kind == TaskKind::Record) {
        if (!session_.completeRecord(result.feedback.event_id)) {
            emit stateChanged();
            setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
                "ReviewController",
                "Evidence receipt was invalid; pair retained"
            )));
            return;
        }
        emit stateChanged();
        setStatusMessage(comparison_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Preference evidence recorded · sequence %1 · model not active"
            ),
            {result.feedback.sequence}
        ));
        emit recorded();
        return;
    }

    if (!session_.completeForget(result.forget.target_event_id)) {
        emit stateChanged();
        setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Forget receipt was invalid; evidence retained"
        )));
        return;
    }
    emit stateChanged();
    setStatusMessage(comparison_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Latest evidence forgotten non-destructively · source event retained"
    )));
    emit forgotten();
}

void ReviewComparisonCoordinator::setStatusMessage(
    LocalizedUiMessage status
) {
    if (status_message_ == status) {
        return;
    }
    status_message_ = std::move(status);
    emit statusTextChanged();
}

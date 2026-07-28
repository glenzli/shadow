#include "review_library_organization_coordinator.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage organization_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewLibraryOrganizationCoordinator::ReviewLibraryOrganizationCoordinator(
    Operations operations,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)) {
    if (!operations_.current || !operations_.mutate || !operations_.project) {
        throw std::invalid_argument(
            "complete Review Library organization operations are required"
        );
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLibraryOrganizationCoordinator::finishTask
    );
}

ReviewLibraryOrganizationCoordinator::~ReviewLibraryOrganizationCoordinator() {
    watcher_.waitForFinished();
}

bool ReviewLibraryOrganizationCoordinator::busy() const noexcept {
    return task_running_;
}

LocalizedUiMessage
ReviewLibraryOrganizationCoordinator::statusMessage() const {
    return status_message_;
}

void ReviewLibraryOrganizationCoordinator::setColorLabel(
    const QString& photo_id,
    const QString& color_label,
    const bool admitted
) {
    if (!admitted || task_running_) {
        return;
    }
    const QString normalized = color_label.trimmed().toLower();
    const auto current = operations_.current(photo_id);
    if (!current) {
        setStatus(organization_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Select a loaded photo before changing its color label"
        )));
        return;
    }
    if (current->color_label == normalized) {
        return;
    }
    startTask(photo_id, current->liked, normalized);
}

void ReviewLibraryOrganizationCoordinator::setLiked(
    const QString& photo_id,
    const bool liked,
    const bool admitted
) {
    if (!admitted || task_running_) {
        return;
    }
    const auto current = operations_.current(photo_id);
    if (!current) {
        setStatus(organization_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Select a loaded photo before changing its Like state"
        )));
        return;
    }
    if (current->liked == liked) {
        return;
    }
    startTask(photo_id, liked, current->color_label);
}

void ReviewLibraryOrganizationCoordinator::retranslateUi() {
    if (!status_message_.isEmpty()) {
        emit statusMessageChanged();
    }
}

ReviewLibraryOrganizationCoordinator::TaskResult
ReviewLibraryOrganizationCoordinator::runTask(
    std::function<BackendPhotoLibraryState(
        const QString& photo_id,
        bool liked,
        const QString& color_label
    )> operation,
    QString photo_id,
    const bool liked,
    QString color_label
) {
    TaskResult result;
    result.requested_photo_id = photo_id;
    try {
        result.state = operation(photo_id, liked, color_label);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewLibraryOrganizationCoordinator::startTask(
    const QString& photo_id,
    const bool liked,
    const QString& color_label
) {
    task_running_ = true;
    setStatus(organization_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Updating Library organization…"
    )));
    watcher_.setFuture(QtConcurrent::run(
        runTask,
        operations_.mutate,
        photo_id,
        liked,
        color_label
    ));
}

void ReviewLibraryOrganizationCoordinator::finishTask() {
    const TaskResult result = watcher_.result();
    task_running_ = false;
    if (!result.error.isEmpty()) {
        setStatus(organization_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not update Library state · %1"
            ),
            {result.error}
        ));
        return;
    }
    if (result.state.photo_id.isEmpty()
        || result.state.photo_id != result.requested_photo_id) {
        setStatus(organization_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Library state receipt was invalid"
        )));
        return;
    }
    static_cast<void>(operations_.project(result.state));
    emit stateProjected(
        result.state.photo_id,
        result.state.liked,
        result.state.color_label
    );
    setStatus(organization_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Library organization updated"
    )));
}

void ReviewLibraryOrganizationCoordinator::setStatus(
    LocalizedUiMessage status
) {
    status_message_ = std::move(status);
    emit statusMessageChanged();
}

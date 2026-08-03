#include "edit_persistence_task_coordinator.hpp"

#include <QtGlobal>

#include <utility>

EditPersistenceTaskCoordinator::EditPersistenceTaskCoordinator(
    QObject& context,
    FinishedCallback finished_callback
) {
    QObject::connect(
        &watcher_,
        &QFutureWatcher<EditStateTaskResult>::finished,
        &context,
        [callback = std::move(finished_callback)] { callback(); }
    );
}

bool EditPersistenceTaskCoordinator::running() const noexcept {
    return running_;
}

bool EditPersistenceTaskCoordinator::futureRunning() const noexcept {
    return watcher_.isRunning();
}

EditStateTaskKind EditPersistenceTaskCoordinator::kind() const noexcept {
    return kind_;
}

void EditPersistenceTaskCoordinator::start(
    const EditStateTaskKind kind,
    QFuture<EditStateTaskResult> future
) {
    Q_ASSERT(!running_);
    Q_ASSERT(!watcher_.isRunning());
    kind_ = kind;
    running_ = true;
    watcher_.setFuture(std::move(future));
}

EditStateTaskResult EditPersistenceTaskCoordinator::complete() {
    Q_ASSERT(running_);
    Q_ASSERT(watcher_.isFinished());
    EditStateTaskResult result = watcher_.result();
    running_ = false;
    kind_ = EditStateTaskKind::Open;
    return result;
}

void EditPersistenceTaskCoordinator::waitForFinished() {
    watcher_.waitForFinished();
}

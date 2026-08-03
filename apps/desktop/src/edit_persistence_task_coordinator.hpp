#pragma once

#include "edit_task_runner.hpp"

#include <QFuture>
#include <QFutureWatcher>
#include <QObject>

#include <functional>

// Owns the one asynchronous Catalog edit-state task slot. Admission, task
// identity, completion delivery, and shutdown waiting stay together so the
// QML-facing EditController does not expose or duplicate watcher state.
class EditPersistenceTaskCoordinator final {
  public:
    using FinishedCallback = std::function<void()>;

    EditPersistenceTaskCoordinator(QObject& context, FinishedCallback finished_callback);

    EditPersistenceTaskCoordinator(const EditPersistenceTaskCoordinator&) = delete;
    EditPersistenceTaskCoordinator& operator=(const EditPersistenceTaskCoordinator&) = delete;
    EditPersistenceTaskCoordinator(EditPersistenceTaskCoordinator&&) = delete;
    EditPersistenceTaskCoordinator& operator=(EditPersistenceTaskCoordinator&&) = delete;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool futureRunning() const noexcept;
    [[nodiscard]] EditStateTaskKind kind() const noexcept;

    void start(EditStateTaskKind kind, QFuture<EditStateTaskResult> future);
    [[nodiscard]] EditStateTaskResult complete();
    void waitForFinished();

  private:
    QFutureWatcher<EditStateTaskResult> watcher_;
    EditStateTaskKind kind_ = EditStateTaskKind::Open;
    bool running_ = false;
};

#include "edit_persistence_state.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "edit persistence state test failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QObject context;
    QEventLoop debounce_loop;
    int timeout_count = 0;
    EditPersistenceState state(context, [&] {
        ++timeout_count;
        debounce_loop.quit();
    });

    if (!require(state.requestAutosave(), "the first edit requests autosave")
        || !require(!state.requestAutosave(), "repeated edits coalesce into one request")) {
        return EXIT_FAILURE;
    }
    state.captureAutosaveSnapshot(17);
    if (!require(
            state.autosaveSnapshotRevision() == 17,
            "the in-flight autosave snapshot retains its exact revision"
        )) {
        return EXIT_FAILURE;
    }

    state.scheduleAutosave(0);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &debounce_loop, &QEventLoop::quit);
    timeout.start(5000);
    debounce_loop.exec();
    if (!require(timeout_count == 1, "the debounce publishes one terminal timeout")) {
        return EXIT_FAILURE;
    }

    const LocalizedUiMessage failure("EditController", "Autosave failed");
    if (!require(state.setAutosaveFailure(failure), "a new autosave failure is retained")
        || !require(!state.setAutosaveFailure(failure), "an identical failure is not republished")
        || !require(state.autosaveFailed(), "failure state remains queryable")
        || !require(state.clearAutosaveFailure(), "explicit recovery clears the failure")) {
        return EXIT_FAILURE;
    }

    state.queuePhotoOpen({
        .photo_id = QStringLiteral("first"),
        .representation_id = QStringLiteral("raw"),
        .source_path = QStringLiteral("/first.raw"),
        .title = QStringLiteral("First"),
    });
    state.queuePhotoOpen({
        .photo_id = QStringLiteral("latest"),
        .representation_id = QStringLiteral("raw"),
        .source_path = QStringLiteral("/latest.raw"),
        .title = QStringLiteral("Latest"),
    });
    const std::optional<PendingPhotoOpen> pending_photo = state.takePendingPhotoOpen();
    if (!require(
            pending_photo.has_value() && pending_photo->photo_id == QStringLiteral("latest"),
            "the newest deferred photo selection wins"
        )) {
        return EXIT_FAILURE;
    }

    state.queueVersionSave(QStringLiteral("Named version"));
    const std::optional<QString> pending_save = state.takePendingVersionSave();
    state.queueVersionLoad(QStringLiteral("commit-42"));
    state.requestPhotoClose();
    state.requestApplicationClose();
    if (!require(
            pending_save == QStringLiteral("Named version"),
            "a deferred named version is consumed exactly once"
        )
        || !require(
            state.hasPendingVersionLoad() && state.hasDeferredCompletionAction(),
            "version checkout and close intent remain serialized behind autosave"
        )
        || !require(
            state.clearPendingVersionActions(),
            "terminal recovery clears pending version actions atomically"
        )) {
        return EXIT_FAILURE;
    }

    state.clearPhotoClose();
    state.cancelApplicationClose();
    (void)state.clearAutosaveRequest();
    state.resetAutosaveSnapshot();
    if (!require(
            !state.hasDeferredCompletionAction() && !state.autosaveRequested()
                && state.autosaveSnapshotRevision() == 0,
            "terminal cleanup returns the persistence state to idle"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

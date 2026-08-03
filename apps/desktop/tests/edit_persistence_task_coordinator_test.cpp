#include "edit_persistence_task_coordinator.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>
#include <QtConcurrentRun>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "edit persistence task coordinator test failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QObject context;
    QEventLoop completion_loop;
    int completion_count = 0;
    EditPersistenceTaskCoordinator coordinator(context, [&] {
        ++completion_count;
        completion_loop.quit();
    });

    coordinator.start(
        EditStateTaskKind::Autosave,
        QtConcurrent::run([] {
            EditStateTaskResult result;
            result.kind = EditStateTaskKind::Autosave;
            result.photo_generation = 42;
            return result;
        })
    );

    if (!require(coordinator.running(), "the admitted task owns the slot")
        || !require(
            coordinator.kind() == EditStateTaskKind::Autosave,
            "the task kind remains stable until completion is consumed"
        )) {
        return EXIT_FAILURE;
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &completion_loop, &QEventLoop::quit);
    timeout.start(5000);
    completion_loop.exec();
    if (!require(completion_count == 1, "completion is delivered exactly once")) {
        return EXIT_FAILURE;
    }

    EditStateTaskResult result = coordinator.complete();
    if (!require(
            result.kind == EditStateTaskKind::Autosave && result.photo_generation == 42,
            "the completed result retains task identity"
        )
        || !require(!coordinator.running(), "consuming completion releases the task slot")
        || !require(
            coordinator.kind() == EditStateTaskKind::Open,
            "an idle slot returns to its neutral task identity"
        )) {
        return EXIT_FAILURE;
    }

    coordinator.waitForFinished();
    return EXIT_SUCCESS;
}

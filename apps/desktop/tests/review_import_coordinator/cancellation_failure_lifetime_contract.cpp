#include "import_coordinator_fixture.hpp"

#include <chrono>
#include <thread>

namespace review_import_test {

void run_cancellation_failure_lifetime_contracts() {
    {
        auto state = std::make_shared<ImportBackendState>();
        state->report = {
            .folder_path = QStringLiteral("/tmp/cancelled"),
            .inserted = 3,
            .decode_cancelled = 2,
            .issue_count = 1,
            .cancelled = true,
        };
        ReviewImportCoordinator coordinator(operations(state));
        require(
            coordinator.start(QUrl::fromLocalFile("/tmp/cancelled"), true),
            "cancellation fixture starts"
        );
        wait_for_scan_entry(state);
        require(
            coordinator.cancel()
                && coordinator.progress()
                       .value(QStringLiteral("phase"))
                       .toString()
                    == QStringLiteral("cancelling"),
            "accepted cancellation is immediately visible"
        );
        {
            std::lock_guard lock(state->mutex);
            require(
                state->cancelled_ids == QVector<quint64>{1},
                "cancellation targets the active identity exactly once"
            );
        }
        release_scan(state);
        wait_until(
            [&coordinator]() { return !coordinator.scanning(); },
            "cancelled import reaches a terminal state"
        );
        require(
            coordinator.readyStatusMessage(3, 3, false)
                .translated()
                .contains(QStringLiteral("Import cancelled")),
            "terminal cancellation keeps a localized durable summary"
        );
    }

    {
        auto state = std::make_shared<ImportBackendState>();
        state->fail_begin = true;
        ReviewImportCoordinator coordinator(operations(state));
        require(
            !coordinator.start(QUrl::fromLocalFile("/tmp/fail"), true)
                && coordinator.statusMessage()
                       .translated()
                       .contains(QStringLiteral("begin failed")),
            "begin failure allocates no running worker and preserves diagnostics"
        );
        require(
            !coordinator.start(QUrl(QStringLiteral("https://example.test")), true)
                && coordinator.statusMessage()
                       .translated()
                       .contains(QStringLiteral("not a local path")),
            "non-local input is rejected with a localized explanation"
        );
    }

    {
        auto state = std::make_shared<ImportBackendState>();
        state->fail_scan = true;
        ReviewImportCoordinator coordinator(operations(state));
        require(
            coordinator.start(QUrl::fromLocalFile("/tmp/fail-scan"), true),
            "worker failure fixture starts"
        );
        wait_for_scan_entry(state);
        release_scan(state);
        wait_until(
            [&coordinator]() { return !coordinator.scanning(); },
            "worker failure reaches a terminal state"
        );
        require(
            coordinator.progress()
                    .value(QStringLiteral("phase"))
                    .toString()
                == QStringLiteral("failed")
                && coordinator.readyStatusMessage(0, 0, false)
                       .translated()
                       .contains(QStringLiteral("scan failed")),
            "worker diagnostics survive into the terminal ready summary"
        );
    }

    {
        auto state = std::make_shared<ImportBackendState>();
        auto coordinator =
            std::make_unique<ReviewImportCoordinator>(operations(state));
        require(
            coordinator->start(
                QUrl::fromLocalFile("/tmp/lifetime"),
                true
            ),
            "lifetime fixture starts"
        );
        wait_for_scan_entry(state);
        std::thread releaser([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            release_scan(state);
        });
        QElapsedTimer timer;
        timer.start();
        coordinator.reset();
        releaser.join();
        require(
            timer.elapsed() >= 25,
            "destruction requests cancellation and waits for the active worker"
        );
        std::lock_guard lock(state->mutex);
        require(
            state->cancelled_ids == QVector<quint64>{1},
            "destruction cancels the same active identity before waiting"
        );
    }
}

} // namespace review_import_test

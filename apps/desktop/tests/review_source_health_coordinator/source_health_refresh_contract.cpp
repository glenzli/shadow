#include "source_health_fixture.hpp"

#include <QSemaphore>

#include <atomic>

namespace review_source_health_test {
namespace {

void complete_projection_and_pending_refresh_are_owned_together() {
    QSemaphore first_started;
    QSemaphore release_first;
    std::atomic<int> calls = 0;

    ReviewSourceHealthCoordinator coordinator(operations([&]() {
        const int call = ++calls;
        if (call == 1) {
            first_started.release();
            release_first.acquire();
        }
        return QVector<BackendLibrarySourceHealth>{
            {
                .source_id = QStringLiteral("source-final"),
                .source_display_path = QString::fromUtf8("/照片/源"),
                .source_enabled = true,
                .has_latest_completed_scan = true,
                .scan_session_id = QStringLiteral("scan-final"),
                .scan_completed_at_ms = 1'700'000'000'123,
                .known_locations = 31,
                .seen_locations = 29,
                .not_seen_locations = 2,
            },
        };
    }));

    coordinator.refreshSourceHealth();
    require(first_started.tryAcquire(1, 3'000), "first source-health refresh did not start");
    require(coordinator.sourceHealthBusy(), "refresh did not publish busy");

    coordinator.refreshSourceHealth();
    coordinator.refreshSourceHealth();
    require(calls.load() == 1, "pending refresh started a concurrent reader");

    release_first.release();
    wait_until(
        [&]() { return calls.load() == 2 && !coordinator.sourceHealthBusy(); },
        "pending refresh did not coalesce into one follow-up read"
    );

    const QVariantList projection = coordinator.sourceHealth();
    require(projection.size() == 1, "source projection size");
    const QVariantMap source = projection.constFirst().toMap();
    require(
        source.value(QStringLiteral("sourceId")).toString() == QStringLiteral("source-final"),
        "sourceId"
    );
    require(
        source.value(QStringLiteral("sourcePath")).toString() == QString::fromUtf8("/照片/源"),
        "sourcePath UTF-8"
    );
    require(source.value(QStringLiteral("enabled")).toBool(), "enabled");
    require(
        source.value(QStringLiteral("hasLatestCompletedScan")).toBool(),
        "hasLatestCompletedScan"
    );
    require(
        source.value(QStringLiteral("scanSessionId")).toString() == QStringLiteral("scan-final"),
        "scanSessionId"
    );
    require(
        source.value(QStringLiteral("scanCompletedAtMs")).toLongLong() == 1'700'000'000'123,
        "scanCompletedAtMs"
    );
    require(source.value(QStringLiteral("knownLocations")).toULongLong() == 31, "knownLocations");
    require(source.value(QStringLiteral("seenLocations")).toULongLong() == 29, "seenLocations");
    require(
        source.value(QStringLiteral("notSeenLocations")).toULongLong() == 2,
        "notSeenLocations"
    );
}

void source_health_failure_publishes_the_canonical_global_message() {
    ReviewSourceHealthCoordinator coordinator(
        operations([]() -> QVector<BackendLibrarySourceHealth> {
            throw std::runtime_error("health unavailable");
        })
    );

    coordinator.refreshSourceHealth();
    wait_until(
        [&]() { return !coordinator.sourceHealthBusy(); },
        "failed source-health refresh did not settle"
    );
    require(
        coordinator.globalStatusMessage().translated()
            == QStringLiteral("Could not load Library source health · health unavailable"),
        "source-health failure status"
    );
}

void removing_a_source_is_async_and_refreshes_the_projection() {
    QSemaphore remove_started;
    QSemaphore release_remove;
    std::atomic<int> health_calls = 0;
    std::atomic<int> visibility_changes = 0;
    QString removed_id;
    ReviewSourceHealthCoordinator coordinator(operations(
        [&]() {
            ++health_calls;
            return QVector<BackendLibrarySourceHealth>{};
        },
        {},
        {},
        [&](const QString& source_id) {
            removed_id = source_id;
            remove_started.release();
            release_remove.acquire();
            return true;
        }
    ));
    QObject::connect(
        &coordinator,
        &ReviewSourceHealthCoordinator::libraryVisibilityChanged,
        [&]() { ++visibility_changes; }
    );

    coordinator.removeSource(QStringLiteral("source-1"), QStringLiteral("/photos"));
    require(remove_started.tryAcquire(1, 3'000), "source removal did not start");
    require(coordinator.removeSourceBusy(), "source removal did not publish busy");
    release_remove.release();
    wait_until(
        [&]() {
            return !coordinator.removeSourceBusy() && !coordinator.sourceHealthBusy()
                   && health_calls.load() == 1;
        },
        "source removal did not settle and refresh source health"
    );
    require(removed_id == QStringLiteral("source-1"), "removed source identity");
    require(visibility_changes.load() == 1, "source removal did not invalidate Library visibility");
    require(
        coordinator.globalStatusMessage().translated()
            == QStringLiteral("Removed Library folder · /photos"),
        "source removal status"
    );
}

} // namespace

void run_source_health_refresh_contracts() {
    complete_projection_and_pending_refresh_are_owned_together();
    source_health_failure_publishes_the_canonical_global_message();
    removing_a_source_is_async_and_refreshes_the_projection();
}

} // namespace review_source_health_test

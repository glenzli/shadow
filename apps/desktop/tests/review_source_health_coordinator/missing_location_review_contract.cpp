#include "source_health_fixture.hpp"

#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>

#include <atomic>
#include <utility>
#include <vector>

namespace review_source_health_test {
namespace {

void switched_review_rejects_stale_page_then_preserves_keyset_append() {
    QSemaphore first_started;
    QSemaphore release_first;
    std::atomic<int> calls = 0;
    QMutex request_mutex;
    std::vector<std::pair<QString, QString>> requests;

    ReviewSourceHealthCoordinator coordinator(operations(
        {},
        [&](const QString& scan_id, const QString& after, const std::uint32_t limit) {
            require(limit == 24, "missing-location page bound");
            {
                QMutexLocker lock(&request_mutex);
                requests.emplace_back(scan_id, after);
            }
            const int call = ++calls;
            if (call == 1) {
                first_started.release();
                release_first.acquire();
                return BackendMissingSourceLocationPage{
                    .has_scan = true,
                    .items = {location(QStringLiteral("stale"))},
                    .has_more = false,
                };
            }
            if (call == 2) {
                return BackendMissingSourceLocationPage{
                    .has_scan = true,
                    .items = {location(QStringLiteral("fresh-first"))},
                    .has_more = true,
                    .next_location_id = QStringLiteral("fresh-cursor"),
                };
            }
            return BackendMissingSourceLocationPage{
                .has_scan = true,
                .items = {location(QStringLiteral("fresh-second"))},
                .has_more = false,
            };
        }
    ));

    coordinator.openMissingLocationReview(QStringLiteral("scan-stale"));
    require(
        first_started.tryAcquire(1, 3'000),
        "initial missing-location page did not start"
    );
    coordinator.openMissingLocationReview(QStringLiteral("scan-fresh"));
    require(
        coordinator.missingLocationScanId() == QStringLiteral("scan-fresh"),
        "review switch did not publish the new scan identity"
    );
    release_first.release();

    wait_until(
        [&]() { return calls.load() == 2 && !coordinator.missingLocationsBusy(); },
        "switched review did not load its replacement page"
    );
    QVariantList projection = coordinator.missingLocations();
    require(projection.size() == 1, "replacement page size");
    require(
        projection.constFirst()
                .toMap()
                .value(QStringLiteral("locationId"))
                .toString()
            == QStringLiteral("fresh-first-location"),
        "stale page entered the replacement review"
    );
    require(
        coordinator.missingLocationsHasMore(),
        "replacement page lost its continuation"
    );

    coordinator.loadMoreMissingLocations();
    wait_until(
        [&]() { return calls.load() == 3 && !coordinator.missingLocationsBusy(); },
        "missing-location continuation did not settle"
    );
    projection = coordinator.missingLocations();
    require(projection.size() == 2, "continuation did not append");
    const QVariantMap second = projection.at(1).toMap();
    require(
        second.value(QStringLiteral("locationId")).toString()
            == QStringLiteral("fresh-second-location"),
        "locationId"
    );
    require(
        second.value(QStringLiteral("photoId")).toString()
            == QStringLiteral("fresh-second-photo"),
        "photoId"
    );
    require(
        second.value(QStringLiteral("title")).toString()
            == QStringLiteral("fresh-second-title"),
        "title"
    );
    require(
        second.value(QStringLiteral("sourcePath")).toString()
            == QStringLiteral("/former-source/fresh-second.nef"),
        "sourcePath"
    );
    require(
        second.value(QStringLiteral("hasCapturedAt")).toBool(),
        "hasCapturedAt"
    );
    require(
        second.value(QStringLiteral("capturedAtUnixSeconds")).toLongLong()
            == 1'700'000'123,
        "capturedAtUnixSeconds"
    );
    require(
        second.value(QStringLiteral("cameraKey")).toString()
            == QStringLiteral("fresh-second-camera"),
        "cameraKey"
    );
    require(
        second.value(QStringLiteral("lastSeenAtMs")).toLongLong()
            == 1'700'000'456'789,
        "lastSeenAtMs"
    );
    require(
        !coordinator.missingLocationsHasMore(),
        "terminal continuation retained has-more"
    );

    QMutexLocker lock(&request_mutex);
    require(requests.size() == 3, "missing-location request count");
    require(
        requests.at(0)
            == std::pair{
                QStringLiteral("scan-stale"),
                QString{},
            },
        "initial request identity"
    );
    require(
        requests.at(1)
            == std::pair{
                QStringLiteral("scan-fresh"),
                QString{},
            },
        "replacement request identity"
    );
    require(
        requests.at(2)
            == std::pair{
                QStringLiteral("scan-fresh"),
                QStringLiteral("fresh-cursor"),
            },
        "continuation cursor"
    );
}

void closing_review_discards_an_in_flight_page() {
    QSemaphore started;
    QSemaphore release;
    ReviewSourceHealthCoordinator coordinator(operations(
        {},
        [&](const QString&, const QString&, const std::uint32_t) {
            started.release();
            release.acquire();
            return BackendMissingSourceLocationPage{
                .has_scan = true,
                .items = {location(QStringLiteral("closed"))},
                .has_more = true,
                .next_location_id = QStringLiteral("closed-cursor"),
            };
        }
    ));

    coordinator.openMissingLocationReview(QStringLiteral("scan-close"));
    require(
        started.tryAcquire(1, 3'000),
        "close contract page did not start"
    );
    coordinator.closeMissingLocationReview();
    release.release();
    wait_until(
        [&]() { return !coordinator.missingLocationsBusy(); },
        "closed review worker did not settle"
    );
    require(
        coordinator.missingLocationScanId().isEmpty(),
        "closed review retained scan identity"
    );
    require(
        coordinator.missingLocations().isEmpty(),
        "closed review accepted its stale page"
    );
    require(
        !coordinator.missingLocationsHasMore(),
        "closed review retained a stale continuation"
    );
}

void missing_review_failure_publishes_the_canonical_global_message() {
    ReviewSourceHealthCoordinator coordinator(operations(
        {},
        [](const QString&, const QString&, const std::uint32_t)
            -> BackendMissingSourceLocationPage {
            throw std::runtime_error("page unavailable");
        }
    ));

    coordinator.openMissingLocationReview(QStringLiteral("scan-error"));
    wait_until(
        [&]() { return !coordinator.missingLocationsBusy(); },
        "failed missing-location page did not settle"
    );
    require(
        coordinator.globalStatusMessage().translated()
            == QStringLiteral(
                "Could not load source scan review · page unavailable"
            ),
        "missing-location failure status"
    );
}

} // namespace

void run_missing_location_review_contracts() {
    switched_review_rejects_stale_page_then_preserves_keyset_append();
    closing_review_discards_an_in_flight_page();
    missing_review_failure_publishes_the_canonical_global_message();
}

} // namespace review_source_health_test

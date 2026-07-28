#include "source_health_fixture.hpp"

#include <QSemaphore>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace review_source_health_test {
namespace {

void relink_owns_admission_arguments_receipt_and_status() {
    QSemaphore started;
    QSemaphore release;
    QString received_scan_id;
    QString received_location_id;
    QString received_path;
    std::atomic<int> calls = 0;

    ReviewSourceHealthCoordinator coordinator(operations(
        {},
        {},
        [&](const QString& scan_id, const QString& location_id, const QString& path) {
            ++calls;
            received_scan_id = scan_id;
            received_location_id = location_id;
            received_path = path;
            started.release();
            release.acquire();
            return BackendVerifiedSourceRelinkReceipt{
                .photo_id = QStringLiteral("photo-relinked"),
                .representation_id = QStringLiteral("representation-relinked"),
                .location_id = QStringLiteral("location-relinked"),
                .display_path = QString::fromUtf8("/新位置/照片.nef"),
            };
        }
    ));

    coordinator.relinkMissingLocation(
        QStringLiteral("location-before-open"),
        QUrl::fromLocalFile(QStringLiteral("/candidate/before-open.nef"))
    );
    require(calls.load() == 0, "relink ran without an open scan review");

    coordinator.openMissingLocationReview(QStringLiteral("  scan-relink  "));
    coordinator.relinkMissingLocation(
        QStringLiteral("  location-relink  "),
        QUrl::fromLocalFile(QStringLiteral("/candidate/moved.nef"))
    );
    require(
        started.tryAcquire(1, 3'000),
        "verified relink worker did not start"
    );
    require(coordinator.relinkBusy(), "relink did not publish busy");
    require(
        coordinator.relinkStatusText()
            == QStringLiteral("Verifying selected source…"),
        "relink verification status"
    );
    coordinator.relinkMissingLocation(
        QStringLiteral("second-location"),
        QUrl::fromLocalFile(QStringLiteral("/candidate/second.nef"))
    );
    require(calls.load() == 1, "relink admitted a concurrent verification");

    release.release();
    wait_until(
        [&]() { return !coordinator.relinkBusy(); },
        "successful relink did not settle"
    );
    require(
        received_scan_id == QStringLiteral("scan-relink"),
        "relink scan identity"
    );
    require(
        received_location_id == QStringLiteral("location-relink"),
        "relink location identity"
    );
    require(
        received_path == QStringLiteral("/candidate/moved.nef"),
        "relink candidate path"
    );
    const QString success =
        QString::fromUtf8("Verified and linked · /新位置/照片.nef");
    require(coordinator.relinkStatusText() == success, "relink receipt status");
    require(
        coordinator.globalStatusMessage().translated() == success,
        "relink receipt global status"
    );
}

void relink_failure_is_retained_for_local_and_global_presentation() {
    ReviewSourceHealthCoordinator coordinator(operations(
        {},
        {},
        [](const QString&, const QString&, const QString&)
            -> BackendVerifiedSourceRelinkReceipt {
            throw std::runtime_error("identity mismatch");
        }
    ));
    coordinator.openMissingLocationReview(QStringLiteral("scan-error"));
    coordinator.relinkMissingLocation(
        QStringLiteral("location-error"),
        QUrl::fromLocalFile(QStringLiteral("/candidate/wrong.nef"))
    );
    wait_until(
        [&]() { return !coordinator.relinkBusy(); },
        "failed relink did not settle"
    );
    const QString failure =
        QStringLiteral("Could not link selected source · identity mismatch");
    require(coordinator.relinkStatusText() == failure, "relink failure status");
    require(
        coordinator.globalStatusMessage().translated() == failure,
        "relink failure global status"
    );
}

void destruction_waits_for_the_active_relink_worker() {
    QSemaphore started;
    QSemaphore release;
    std::atomic<bool> finished = false;
    auto coordinator = std::make_unique<ReviewSourceHealthCoordinator>(
        operations(
            {},
            {},
            [&](const QString&, const QString&, const QString&) {
                started.release();
                release.acquire();
                finished = true;
                return BackendVerifiedSourceRelinkReceipt{
                    .display_path = QStringLiteral("/candidate/waited.nef"),
                };
            }
        )
    );
    coordinator->openMissingLocationReview(QStringLiteral("scan-lifetime"));
    coordinator->relinkMissingLocation(
        QStringLiteral("location-lifetime"),
        QUrl::fromLocalFile(QStringLiteral("/candidate/waited.nef"))
    );
    require(
        started.tryAcquire(1, 3'000),
        "lifetime relink worker did not start"
    );

    std::thread releaser([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        release.release();
    });
    coordinator.reset();
    releaser.join();
    require(finished.load(), "coordinator destruction abandoned its worker");
}

} // namespace

void run_relink_lifetime_contracts() {
    relink_owns_admission_arguments_receipt_and_status();
    relink_failure_is_retained_for_local_and_global_presentation();
    destruction_waits_for_the_active_relink_worker();
}

} // namespace review_source_health_test

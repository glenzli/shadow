#include "review_remote_library_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "remote Library coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate>
void waitUntil(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

[[nodiscard]] BackendRemoteLibrarySnapshot remoteSnapshot() {
    BackendRemoteLibraryPhoto photo;
    photo.server_id = QStringLiteral("server-a");
    photo.remote_photo_id = QStringLiteral("photo-a");
    photo.remote_representation_id = QStringLiteral("representation-a");
    photo.title = QStringLiteral("Remote A.nef");
    photo.has_preview = true;
    photo.preview_path = QStringLiteral("/client-cache/remote-a");
    photo.preview_role = QStringLiteral("embedded_preview");
    photo.preview_width = 640;
    photo.preview_height = 480;
    photo.has_captured_at = true;
    photo.captured_at_unix_seconds = 1'700'000'000;
    photo.camera_make = QStringLiteral("Nikon");
    photo.decision_flag = BackendReviewDecisionFlag::Unflagged;
    photo.color_label = QStringLiteral("none");
    return {
        .has_server = true,
        .server = {
            .server_id = QStringLiteral("server-a"),
            .display_name = QStringLiteral("Studio Mac"),
            .embedded_previews_available = true,
            .generated_proxies_available = true,
            .originals_available = true,
        },
        .photos = {photo},
    };
}

void offline_sync_curation_and_materialization_are_non_blocking_and_identity_safe() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "temporary settings root");
    const QString settings_file = settings_root.filePath(QStringLiteral("preferences.ini"));
    ReviewModel model;
    ReviewItem local;
    local.photo_id = QStringLiteral("local-photo");
    local.representation_id = QStringLiteral("local-representation");
    local.title = QStringLiteral("Local");
    model.replace({local}, 7);

    std::atomic<int> sync_calls = 0;
    std::atomic<int> mutation_calls = 0;
    std::atomic<int> materialize_calls = 0;
    std::atomic<bool> ran_off_main_thread = false;
    const QThread* const main_thread = QThread::currentThread();
    auto snapshot = remoteSnapshot();
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [snapshot, main_thread, &ran_off_main_thread]() {
                ran_off_main_thread.store(
                    QThread::currentThread() != main_thread,
                    std::memory_order_release
                );
                return snapshot;
            },
            .sync = [snapshot, &sync_calls](const QString& address, const QString& token) {
                require(address == QStringLiteral("studio.local:45321"), "synchronized address");
                require(token.size() == 32, "secure token supplied only to worker operation");
                sync_calls.fetch_add(1, std::memory_order_acq_rel);
                return BackendRemoteLibrarySyncResult{
                    .snapshot = snapshot,
                    .page_count = 1,
                    .photo_count = 1,
                    .downloaded_previews = 0,
                    .removed = 0,
                };
            },
            .set_review_state = [&mutation_calls](
                                    const QString& remote_photo_id,
                                    const QString& remote_representation_id,
                                    const BackendReviewDecisionFlag,
                                    const std::uint8_t,
                                    const bool,
                                    const QString&,
                                    const std::int64_t
                                ) {
                require(remote_photo_id == QStringLiteral("photo-a"), "remote photo identity");
                require(
                    remote_representation_id == QStringLiteral("representation-a"),
                    "remote representation identity"
                );
                mutation_calls.fetch_add(1, std::memory_order_acq_rel);
            },
            .materialize = [&materialize_calls](
                               const QString&,
                               const QString& token,
                               const QString& remote_photo_id,
                               const QString& remote_representation_id
                           ) {
                require(token.size() == 32, "materialization receives secure token");
                require(remote_photo_id == QStringLiteral("photo-a"), "materialized photo");
                require(
                    remote_representation_id == QStringLiteral("representation-a"),
                    "materialized representation"
                );
                materialize_calls.fetch_add(1, std::memory_order_acq_rel);
                return BackendRemoteLibraryMaterialization{
                    .local_photo_id = QStringLiteral("local-materialized-photo"),
                    .local_representation_id = QStringLiteral("local-materialized-representation"),
                    .local_source_path = QStringLiteral("/local-cache/photo-a.nef"),
                    .title = QStringLiteral("Remote A.nef"),
                    .reused_existing = false,
                };
            },
        },
        model,
        settings_file,
        makeVolatileSecretStore()
    );

    coordinator.start();
    waitUntil(
        [&coordinator]() { return coordinator.remotePhotoCount() == 1; },
        "offline snapshot completion"
    );
    require(ran_off_main_thread.load(std::memory_order_acquire), "snapshot must run off UI thread");
    require(
        model.rowCount() == 2 && coordinator.hasServer()
            && coordinator.serverName() == QStringLiteral("Studio Mac")
            && coordinator.remotePhotoCount() == 1,
        "offline mirror must append beside the local Catalog row"
    );
    const QString presentation_photo_id = model
                                              .data(
                                                  model.index(1, 0),
                                                  ReviewModel::PhotoIdRole
                                              )
                                              .toString();
    require(
        presentation_photo_id.startsWith(QStringLiteral("remote:server-a:")),
        "remote presentation identity must be namespaced by server"
    );

    require(
        coordinator.saveConnection(
            QStringLiteral("studio.local:45321"),
            QStringLiteral("01234567890123456789012345678901")
        ),
        "save secure connection"
    );
    coordinator.syncNow();
    waitUntil(
        [&coordinator, &sync_calls]() {
            return coordinator.statusCode() == QStringLiteral("synchronized")
                   && sync_calls.load(std::memory_order_acquire) == 1;
        },
        "remote synchronization completion"
    );
    require(
        coordinator.statusCode() == QStringLiteral("synchronized"),
        "successful synchronization status"
    );

    require(
        coordinator.setDecision(presentation_photo_id, BackendReviewDecisionFlag::Picked, 4),
        "remote decision mutation admission"
    );
    require(
        coordinator.setAffinity(presentation_photo_id, true, QStringLiteral("red")),
        "remote affinity mutation admission"
    );
    waitUntil(
        [&mutation_calls]() { return mutation_calls.load(std::memory_order_acquire) == 2; },
        "serialized remote curation persistence"
    );
    require(
        model.data(model.index(1, 0), ReviewModel::DecisionRatingRole).toInt() == 4
            && model.data(model.index(1, 0), ReviewModel::LikedRole).toBool(),
        "remote curation must update the grid optimistically"
    );

    QString ready_photo_id;
    int refresh_requests = 0;
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::remotePhotoReady,
        [&ready_photo_id](const QString& photo_id, const QString&, const QString&, const QString&) {
            ready_photo_id = photo_id;
        }
    );
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::localLibraryRefreshRequested,
        [&refresh_requests]() { ++refresh_requests; }
    );
    coordinator.materializeForEdit(presentation_photo_id);
    waitUntil(
        [&ready_photo_id, &materialize_calls]() {
            return !ready_photo_id.isEmpty()
                   && materialize_calls.load(std::memory_order_acquire) == 1;
        },
        "remote original materialization completion"
    );
    require(
        ready_photo_id == QStringLiteral("local-materialized-photo") && refresh_requests == 1,
        "verified local identity must be emitted and local Library refreshed"
    );
    require(
        model.rowCount() == 1
            && model.data(model.index(0, 0), ReviewModel::PhotoIdRole).toString()
                   == QStringLiteral("local-photo"),
        "materialized remote row must yield to the local Catalog projection"
    );
}

void shutdown_drains_queued_remote_curation() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "shutdown settings root");
    ReviewModel model;
    std::atomic<int> mutation_calls = 0;
    {
        ReviewRemoteLibraryCoordinator coordinator(
            {
                .snapshot = []() { return remoteSnapshot(); },
                .sync = [](const QString&, const QString&) {
                    return BackendRemoteLibrarySyncResult{};
                },
                .set_review_state = [&mutation_calls](
                                        const QString&,
                                        const QString&,
                                        const BackendReviewDecisionFlag,
                                        const std::uint8_t,
                                        const bool,
                                        const QString&,
                                        const std::int64_t
                                    ) {
                    QThread::msleep(10);
                    mutation_calls.fetch_add(1, std::memory_order_acq_rel);
                },
                .materialize = [](const QString&, const QString&, const QString&, const QString&) {
                    return BackendRemoteLibraryMaterialization{};
                },
            },
            model,
            settings_root.filePath(QStringLiteral("preferences.ini")),
            makeVolatileSecretStore()
        );
        coordinator.start();
        waitUntil(
            [&coordinator]() { return coordinator.remotePhotoCount() == 1; },
            "shutdown snapshot completion"
        );
        const QString photo_id = model
                                     .data(model.index(0, 0), ReviewModel::PhotoIdRole)
                                     .toString();
        require(
            coordinator.setDecision(photo_id, BackendReviewDecisionFlag::Picked, 1)
                && coordinator.setDecision(photo_id, BackendReviewDecisionFlag::Picked, 2)
                && coordinator.setDecision(photo_id, BackendReviewDecisionFlag::Picked, 3),
            "queue rapid remote curation before shutdown"
        );
    }
    require(
        mutation_calls.load(std::memory_order_acquire) == 3,
        "coordinator destruction must persist every admitted queued curation mutation"
    );
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    offline_sync_curation_and_materialization_are_non_blocking_and_identity_safe();
    shutdown_drains_queued_remote_curation();
    return EXIT_SUCCESS;
}

#include "review_remote_library_coordinator.hpp"
#include "remote_photo_aggregation.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "remote Library coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void waitUntil(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

[[nodiscard]] int remoteRow(const ReviewModel& model) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.data(model.index(row, 0), ReviewModel::IsRemoteRole).toBool()) {
            return row;
        }
    }
    return -1;
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
        .server =
            {
                .server_id = QStringLiteral("server-a"),
                .display_name = QStringLiteral("Studio Mac"),
                .embedded_previews_available = true,
                .generated_proxies_available = true,
                .originals_available = true,
            },
        .photos = {photo},
    };
}

[[nodiscard]] BackendRemoteLibrarySnapshot remoteSnapshotB() {
    BackendRemoteLibrarySnapshot snapshot = remoteSnapshot();
    snapshot.server.server_id = QStringLiteral("server-b");
    snapshot.server.display_name = QStringLiteral("Travel Mac");
    snapshot.photos.front().server_id = QStringLiteral("server-b");
    snapshot.photos.front().remote_photo_id = QStringLiteral("photo-b");
    snapshot.photos.front().remote_representation_id = QStringLiteral("representation-b");
    snapshot.photos.front().title = QStringLiteral("Remote B.nef");
    snapshot.photos.front().preview_path = QStringLiteral("/client-cache/remote-b");
    return snapshot;
}

void offline_sync_curation_and_materialization_are_non_blocking_and_identity_safe() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "temporary settings root");
    const QString settings_file = settings_root.filePath(QStringLiteral("preferences.ini"));
    {
        QSettings settings(settings_file, QSettings::IniFormat);
        settings.setValue(
            QStringLiteral("remote_library/server_address"),
            QStringLiteral("studio.local:45321")
        );
        settings.setValue(QStringLiteral("remote_library/token_stored"), false);
    }
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
            .snapshot =
                [snapshot, main_thread, &ran_off_main_thread](const QString& connection_id) {
                    require(!connection_id.isEmpty(), "stable connection identity on cache read");
                    ran_off_main_thread.store(
                        QThread::currentThread() != main_thread,
                        std::memory_order_release
                    );
                    return snapshot;
                },
            .sync =
                [snapshot, &sync_calls](
                    const QString& connection_id,
                    const QString& address,
                    const QString& token
                ) {
                    require(!connection_id.isEmpty(), "stable connection identity on sync");
                    require(
                        address == QStringLiteral("studio.local:45321"),
                        "synchronized address"
                    );
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
            .set_review_state =
                [&mutation_calls](
                    const QString& connection_id,
                    const QString& remote_photo_id,
                    const QString& remote_representation_id,
                    const BackendReviewDecisionFlag,
                    const std::uint8_t,
                    const bool,
                    const QString&,
                    const std::int64_t
                ) {
                    require(!connection_id.isEmpty(), "stable connection identity on mutation");
                    require(remote_photo_id == QStringLiteral("photo-a"), "remote photo identity");
                    require(
                        remote_representation_id == QStringLiteral("representation-a"),
                        "remote representation identity"
                    );
                    mutation_calls.fetch_add(1, std::memory_order_acq_rel);
                },
            .materialize =
                [&materialize_calls](
                    const QString& connection_id,
                    const QString&,
                    const QString& token,
                    const QString& remote_photo_id,
                    const QString& remote_representation_id
                ) {
                    require(!connection_id.isEmpty(), "stable connection identity on materialize");
                    require(token.size() == 32, "materialization receives secure token");
                    require(remote_photo_id == QStringLiteral("photo-a"), "materialized photo");
                    require(
                        remote_representation_id == QStringLiteral("representation-a"),
                        "materialized representation"
                    );
                    materialize_calls.fetch_add(1, std::memory_order_acq_rel);
                    return BackendRemoteLibraryMaterialization{
                        .local_photo_id = QStringLiteral("local-materialized-photo"),
                        .local_representation_id =
                            QStringLiteral("local-materialized-representation"),
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
    const int projected_remote_row = remoteRow(model);
    const QString presentation_photo_id = model
                                              .data(
                                                  model.index(projected_remote_row, 0),
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
        model.data(model.index(projected_remote_row, 0), ReviewModel::DecisionRatingRole).toInt()
                == 4
            && model.data(model.index(projected_remote_row, 0), ReviewModel::LikedRole).toBool(),
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
        model.rowCount() == 2 && remoteRow(model) >= 0
            && model.data(model.index(remoteRow(model), 0), ReviewModel::PhotoIdRole).toString()
                   == presentation_photo_id
            && model.data(model.index(remoteRow(model), 0), ReviewModel::IsRemoteRole).toBool()
            && model
                   .data(model.index(remoteRow(model), 0), ReviewModel::RemoteOriginalCachedRole)
                   .toBool(),
        "a cached original must remain a remote-origin row with explicit local residency"
    );
}

void shutdown_drains_queued_remote_curation() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "shutdown settings root");
    const QString settings_file = settings_root.filePath(QStringLiteral("preferences.ini"));
    {
        QSettings settings(settings_file, QSettings::IniFormat);
        settings.setValue(
            QStringLiteral("remote_library/server_address"),
            QStringLiteral("studio.local:45321")
        );
        settings.setValue(QStringLiteral("remote_library/token_stored"), false);
    }
    ReviewModel model;
    std::atomic<int> mutation_calls = 0;
    {
        ReviewRemoteLibraryCoordinator coordinator(
            {
                .snapshot = [](const QString&) { return remoteSnapshot(); },
                .sync = [](const QString&,
                           const QString&,
                           const QString&) { return BackendRemoteLibrarySyncResult{}; },
                .set_review_state =
                    [&mutation_calls](
                        const QString&,
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
                .materialize = [](const QString&,
                                  const QString&,
                                  const QString&,
                                  const QString&,
                                  const QString&) { return BackendRemoteLibraryMaterialization{}; },
            },
            model,
            settings_file,
            makeVolatileSecretStore()
        );
        coordinator.start();
        waitUntil(
            [&coordinator]() { return coordinator.remotePhotoCount() == 1; },
            "shutdown snapshot completion"
        );
        const QString photo_id = model.data(model.index(0, 0), ReviewModel::PhotoIdRole).toString();
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

void multiple_connections_keep_independent_identity_and_projection() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "multi-connection settings root");
    ReviewModel model;
    std::atomic<int> sync_calls = 0;
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
            .sync =
                [&sync_calls](
                    const QString& connection_id,
                    const QString& address,
                    const QString& token
                ) {
                    require(!connection_id.isEmpty(), "multi-connection stable identity");
                    require(token.size() == 32, "multi-connection secret routing");
                    sync_calls.fetch_add(1, std::memory_order_acq_rel);
                    return BackendRemoteLibrarySyncResult{
                        .snapshot = address.startsWith(QStringLiteral("travel")) ? remoteSnapshotB()
                                                                                 : remoteSnapshot(),
                        .page_count = 1,
                        .photo_count = 1,
                    };
                },
            .set_review_state = [](const QString&,
                                   const QString&,
                                   const QString&,
                                   BackendReviewDecisionFlag,
                                   std::uint8_t,
                                   bool,
                                   const QString&,
                                   std::int64_t) {},
            .materialize =
                [](const QString&, const QString&, const QString&, const QString&, const QString&) {
                    return BackendRemoteLibraryMaterialization{};
                },
        },
        model,
        settings_root.filePath(QStringLiteral("preferences.ini")),
        makeVolatileSecretStore()
    );

    const QString studio_id = coordinator.saveConnection(
        {},
        QStringLiteral("studio.local:45321"),
        QStringLiteral("01234567890123456789012345678901")
    );
    require(!studio_id.isEmpty(), "first remote Library connection");
    waitUntil(
        [&coordinator, &sync_calls]() {
            return !coordinator.busy() && sync_calls.load(std::memory_order_acquire) == 1;
        },
        "first remote Library synchronization"
    );

    const QString travel_id = coordinator.saveConnection(
        {},
        QStringLiteral("travel.local:45321"),
        QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEF")
    );
    require(!travel_id.isEmpty() && travel_id != studio_id, "second stable connection identity");
    waitUntil(
        [&coordinator, &sync_calls]() {
            return !coordinator.busy() && sync_calls.load(std::memory_order_acquire) == 2;
        },
        "second remote Library synchronization"
    );

    const QVariantList connections = coordinator.connections();
    require(
        connections.size() == 2 && coordinator.remotePhotoCount() == 2 && model.rowCount() == 2,
        "both remote Libraries must remain independently visible (connections="
            + std::to_string(connections.size())
            + ", remote photos=" + std::to_string(coordinator.remotePhotoCount())
            + ", rows=" + std::to_string(model.rowCount()) + ")"
    );
    require(
        coordinator.removeConnection(studio_id) && coordinator.connections().size() == 1
            && coordinator.remotePhotoCount() == 1 && model.rowCount() == 1,
        "removing one remote Library must preserve the other projection"
    );
}

void exact_original_identity_merges_server_copies_and_retains_sources() {
    BackendRemoteLibrarySnapshot studio = remoteSnapshot();
    studio.server.originals_available = false;
    studio.photos.front().has_original_identity = true;
    studio.photos.front().original_digest_hex = QString(64, QLatin1Char('a'));
    studio.photos.front().representation_count = 2;
    studio.photos.front().source_location_count = 2;
    studio.photos.front().has_raw_representation = true;
    studio.photos.front().has_raster_representation = true;

    BackendRemoteLibrarySnapshot travel = remoteSnapshotB();
    travel.photos.front().has_original_identity = true;
    travel.photos.front().original_digest_hex = QString(64, QLatin1Char('A'));
    travel.photos.front().source_location_count = 1;
    travel.photos.front().has_raw_representation = true;

    const RemotePhotoAggregateMap aggregates = aggregateRemotePhotos({
        {QStringLiteral("connection-studio"), studio},
        {QStringLiteral("connection-travel"), travel},
    });
    require(aggregates.size() == 1, "exact content identity collapses server copies");
    const auto aggregate = aggregates.constFind(
        QStringLiteral("remote-content:") + QString(64, QLatin1Char('a'))
    );
    require(aggregate != aggregates.cend(), "content identity is the presentation key");
    require(
        aggregate->sources.size() == 2 && aggregate->representation_count == 2
            && aggregate->source_location_count == 3 && aggregate->has_raw_representation
            && aggregate->has_raster_representation,
        "logical photo retains every representation and source capability"
    );

    studio.photos.front().has_cached_original = true;
    const auto cached = aggregateRemotePhotos({
        {QStringLiteral("connection-studio"), studio},
        {QStringLiteral("connection-travel"), travel},
    });
    const auto cached_aggregate = cached.constFind(
        QStringLiteral("remote-content:") + QString(64, QLatin1Char('a'))
    );
    require(
        cached_aggregate != cached.cend() && cached_aggregate->has_cached_original
            && cached_aggregate->preferredSource() != nullptr
            && cached_aggregate->preferredSource()->connection_id
                   == QStringLiteral("connection-studio"),
        "a verified cached original is residency evidence and the preferred offline edit source"
    );
    require(
        aggregate->preferredSource() != nullptr
            && aggregate->preferredSource()->connection_id == QStringLiteral("connection-travel"),
        "an original-capable source is preferred over an offline proxy"
    );

    studio.photos.front().has_original_identity = false;
    studio.photos.front().original_digest_hex.clear();
    travel.photos.front().has_original_identity = false;
    travel.photos.front().original_digest_hex.clear();
    require(
        aggregateRemotePhotos({
            {QStringLiteral("connection-studio"), studio},
            {QStringLiteral("connection-travel"), travel},
        }).size()
            == 2,
        "unprepared originals never merge from filenames or metadata alone"
    );
}

void connection_store_preserves_stable_ids_and_legacy_migration() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "connection store settings root");
    const QString settings_file = settings_root.filePath(QStringLiteral("preferences.ini"));
    QString first_id;
    QString second_id;
    {
        RemoteLibraryConnectionStore store(settings_file);
        first_id = store.add(QStringLiteral("studio.local:45321"), true);
        second_id = store.add(QStringLiteral("travel.local:45321"), true);
        require(first_id != second_id, "new Libraries receive distinct stable identities");
    }
    {
        RemoteLibraryConnectionStore reopened(settings_file);
        require(
            reopened.connections().size() == 2 && reopened.connections().at(0).id == first_id
                && reopened.connections().at(1).id == second_id,
            "connection order and stable identities survive application restart"
        );
    }

    const QString legacy_file = settings_root.filePath(QStringLiteral("legacy.ini"));
    {
        QSettings legacy(legacy_file, QSettings::IniFormat);
        legacy.setValue(
            QStringLiteral("remote_library/server_address"),
            QStringLiteral("legacy.local:45321")
        );
        legacy.setValue(QStringLiteral("remote_library/token_stored"), true);
    }
    RemoteLibraryConnectionStore migrated(legacy_file);
    require(
        migrated.connections().size() == 1 && migrated.connections().front().uses_legacy_secret,
        "the former single remote Library becomes one stable connection"
    );
}

void authorization_rejection_is_actionable() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "authorization rejection settings root");
    ReviewModel model;
    ReviewRemoteLibraryCoordinator rejected(
        {
            .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
            .sync = [](const QString&, const QString&, const QString&)
                -> BackendRemoteLibrarySyncResult {
                throw std::runtime_error("remote Library request failed: authorization failed");
            },
            .set_review_state = [](const QString&,
                                   const QString&,
                                   const QString&,
                                   BackendReviewDecisionFlag,
                                   std::uint8_t,
                                   bool,
                                   const QString&,
                                   std::int64_t) {},
            .materialize =
                [](const QString&, const QString&, const QString&, const QString&, const QString&) {
                    return BackendRemoteLibraryMaterialization{};
                },
        },
        model,
        settings_root.filePath(QStringLiteral("preferences.ini")),
        makeVolatileSecretStore()
    );
    const QString rejected_id = rejected.saveConnection(
        {},
        QStringLiteral("192.168.1.20:37641"),
        QString(32, QLatin1Char('r'))
    );
    require(!rejected_id.isEmpty(), "rejected remote connection admission");
    waitUntil([&rejected]() { return !rejected.busy(); }, "rejected remote synchronization");
    require(
        rejected.statusCode() == QStringLiteral("authorization-failed")
            && rejected.diagnosticText().contains(QStringLiteral("authorization failed")),
        "authorization rejection has an actionable status and preserves diagnostics"
    );
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    offline_sync_curation_and_materialization_are_non_blocking_and_identity_safe();
    shutdown_drains_queued_remote_curation();
    multiple_connections_keep_independent_identity_and_projection();
    exact_original_identity_merges_server_copies_and_retains_sources();
    connection_store_preserves_stable_ids_and_legacy_migration();
    authorization_rejection_is_actionable();
    return EXIT_SUCCESS;
}

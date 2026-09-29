#include "remote_photo_aggregation.hpp"
#include "review_remote_library_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

[[nodiscard]] QString cachedPreview(const QString& name) {
    static QTemporaryDir cache;
    require(cache.isValid(), "preview cache fixture is available");
    const QString path = cache.filePath(name);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "preview cache fixture is writable");
    file.write("fixture");
    return path;
}

[[nodiscard]] BackendRemoteLibrarySnapshot remoteSnapshot() {
    BackendRemoteLibraryPhoto photo;
    photo.server_id = QStringLiteral("server-a");
    photo.remote_photo_id = QStringLiteral("photo-a");
    photo.remote_representation_id = QStringLiteral("representation-a");
    photo.title = QStringLiteral("Remote A.nef");
    photo.has_preview = true;
    photo.preview_path = cachedPreview(QStringLiteral("remote-a"));
    photo.preview_role = QStringLiteral("embedded_preview");
    photo.preview_width = 640;
    photo.preview_height = 480;
    photo.preview_auto_transform = true;
    photo.metadata_schema_version = 1;
    photo.has_captured_at = true;
    photo.captured_at_unix_seconds = 1'700'000'000;
    photo.camera_make = QStringLiteral("Nikon");
    photo.has_focal_length_35mm = true;
    photo.focal_length_35mm = 50.0;
    photo.has_image_dimensions = true;
    photo.image_width = 4'000;
    photo.image_height = 6'000;
    photo.has_orientation = true;
    photo.orientation = 6;
    photo.has_coordinates = true;
    photo.latitude_degrees = 35.0;
    photo.longitude_degrees = 139.0;
    photo.has_altitude = true;
    photo.altitude_meters = 12.0;
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
    snapshot.photos.front().preview_path = cachedPreview(QStringLiteral("remote-b"));
    return snapshot;
}

[[nodiscard]] BackendRemoteLibrarySnapshot remoteSnapshotNamed(
    const QString& photo_id,
    const QString& representation_id,
    const QString& title
) {
    BackendRemoteLibrarySnapshot snapshot = remoteSnapshot();
    snapshot.photos.front().remote_photo_id = photo_id;
    snapshot.photos.front().remote_representation_id = representation_id;
    snapshot.photos.front().title = title;
    snapshot.photos.front().preview_path = cachedPreview(photo_id);
    return snapshot;
}

[[nodiscard]] BackendRemoteLibrarySnapshot emptyRemoteSnapshot() {
    BackendRemoteLibrarySnapshot snapshot = remoteSnapshot();
    snapshot.photos.clear();
    return snapshot;
}

[[nodiscard]] BackendRemoteLibrarySnapshot twoPhotoSnapshot() {
    BackendRemoteLibrarySnapshot snapshot = remoteSnapshotNamed(
        QStringLiteral("page-photo-a"),
        QStringLiteral("page-representation-a"),
        QStringLiteral("Page A.nef")
    );
    snapshot.photos.push_back(remoteSnapshotNamed(
                                  QStringLiteral("page-photo-b"),
                                  QStringLiteral("page-representation-b"),
                                  QStringLiteral("Page B.nef")
    )
                                  .photos.front());
    return snapshot;
}

[[nodiscard]] BackendRemoteLibrarySyncStart
syncStart(const std::uint64_t job_id, const BackendRemoteLibrarySnapshot& snapshot) {
    return {
        .job_id = job_id,
        .snapshot = snapshot,
    };
}

[[nodiscard]] BackendRemoteLibrarySyncStep syncComplete(
    const std::uint64_t job_id,
    const BackendRemoteLibrarySnapshot& snapshot,
    const std::uint64_t preview_failures = 0
) {
    return {
        .job_id = job_id,
        .snapshot = snapshot,
        .stage = QStringLiteral("previews"),
        .page_count = 1,
        .photo_count = static_cast<std::uint64_t>(snapshot.photos.size()),
        .preview_completed_count = static_cast<std::uint64_t>(snapshot.photos.size()),
        .downloaded_previews =
            preview_failures == 0 ? static_cast<std::uint64_t>(snapshot.photos.size()) : 0,
        .preview_failures = preview_failures,
        .manifest_complete = true,
        .complete = true,
        .diagnostic =
            preview_failures == 0 ? QString{} : QStringLiteral("fixture preview unavailable"),
    };
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
    const QString materialized_source =
        settings_root.filePath(QStringLiteral("materialized-photo-a.nef"));
    QFile materialized_file(materialized_source);
    require(
        materialized_file.open(QIODevice::WriteOnly)
            && materialized_file.write("materialized-original") == 21,
        "materialized original fixture"
    );
    materialized_file.close();
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
            .begin_sync =
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
                    return syncStart(1, snapshot);
                },
            .sync_step =
                [snapshot](const std::uint64_t job_id) { return syncComplete(job_id, snapshot); },
            .cancel_sync = [](const std::uint64_t) { return true; },
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
                [&materialize_calls, materialized_source](
                    const QString& connection_id,
                    const QString&,
                    const QString& token,
                    const QString& remote_photo_id,
                    const QString& remote_representation_id
                ) {
                    require(!connection_id.isEmpty(), "stable connection identity on materialize");
                    const int call = materialize_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
                    require(
                        call == 1 ? token.size() == 32 : token.isEmpty(),
                        "uncached materialization receives a token while cached reuse does not"
                    );
                    require(remote_photo_id == QStringLiteral("photo-a"), "materialized photo");
                    require(
                        remote_representation_id == QStringLiteral("representation-a"),
                        "materialized representation"
                    );
                    return BackendRemoteLibraryMaterialization{
                        .local_photo_id = QStringLiteral("local-materialized-photo"),
                        .local_representation_id =
                            QStringLiteral("local-materialized-representation"),
                        .local_source_path = materialized_source,
                        .title = QStringLiteral("Remote A.nef"),
                        .reused_existing = false,
                        .inspection_diagnostic = QStringLiteral("fixture provider unavailable"),
                        .metadata_schema_version = 1,
                        .camera_make = QStringLiteral("Nikon inspected"),
                        .has_focal_length_35mm = true,
                        .focal_length_35mm = 52.0,
                        .has_image_dimensions = true,
                        .image_width = 4'000,
                        .image_height = 6'000,
                        .has_orientation = true,
                        .orientation = 6,
                        .has_coordinates = true,
                        .latitude_degrees = 35.0,
                        .longitude_degrees = 139.0,
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
        coordinator.systemCollectionCounts().value(QStringLiteral("all")).toInt() == 1
            && coordinator.systemCollectionCounts().value(QStringLiteral("liked")).toInt() == 0
            && coordinator.systemCollectionCounts().value(QStringLiteral("fiveStar")).toInt() == 0,
        "offline collection counts include remote logical photos without counting local rows"
    );
    require(
        model.rowCount() == 2 && coordinator.hasServer()
            && coordinator.serverName() == QStringLiteral("Studio Mac")
            && coordinator.remotePhotoCount() == 1,
        "offline mirror must append beside the local Catalog row"
    );
    const int projected_remote_row = remoteRow(model);
    const QString presentation_photo_id =
        model.data(model.index(projected_remote_row, 0), ReviewModel::PhotoIdRole).toString();
    const QString presentation_representation_id =
        model.data(model.index(projected_remote_row, 0), ReviewModel::RepresentationIdRole)
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

    int collection_count_notifications = 0;
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::systemCollectionCountsChanged,
        [&collection_count_notifications]() { ++collection_count_notifications; }
    );
    require(
        coordinator.setDecision(presentation_photo_id, BackendReviewDecisionFlag::Picked, 5),
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
                == 5
            && model.data(model.index(projected_remote_row, 0), ReviewModel::LikedRole).toBool(),
        "remote curation must update the grid optimistically"
    );

    require(
        collection_count_notifications == 2,
        "remote rating and Like changes must notify collection badge bindings immediately"
    );
    (void)model.replaceRemoteItems({});
    require(
        coordinator.systemCollectionCounts().value(QStringLiteral("all")).toInt() == 1
            && coordinator.systemCollectionCounts().value(QStringLiteral("liked")).toInt() == 1
            && coordinator.systemCollectionCounts().value(QStringLiteral("fiveStar")).toInt() == 1,
        "curation counts remain global when a local Library scope hides remote rows"
    );
    coordinator.reapplyRemoteItems();

    QString ready_photo_id;
    QVariantMap ready_capture_metadata;
    QVariantMap refreshed_inspection;
    int refresh_requests = 0;
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::remotePhotoReady,
        [&ready_photo_id, &ready_capture_metadata](
            const QString& photo_id,
            const QString&,
            const QString&,
            const QString&,
            const QVariantMap& capture_metadata
        ) {
            ready_photo_id = photo_id;
            ready_capture_metadata = capture_metadata;
        }
    );
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::localLibraryRefreshRequested,
        [&refresh_requests]() { ++refresh_requests; }
    );
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::remoteInspectionChanged,
        [&refreshed_inspection](const QString&, const QVariantMap& inspection) {
            refreshed_inspection = inspection;
        }
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
        ready_capture_metadata.value(QStringLiteral("representationId")).toString()
                == QStringLiteral("local-materialized-representation")
            && ready_capture_metadata.value(QStringLiteral("available")).toBool()
            && ready_capture_metadata.value(QStringLiteral("cameraMake")).toString()
                   == QStringLiteral("Nikon inspected")
            && ready_capture_metadata.value(QStringLiteral("orientation")).toInt() == 6,
        "Precision metadata must be bound to the materialized local representation"
    );
    require(
        refreshed_inspection.value(QStringLiteral("photoId")).toString() == presentation_photo_id
            && refreshed_inspection.value(QStringLiteral("representationId")).toString()
                   == presentation_representation_id
            && refreshed_inspection.value(QStringLiteral("cameraMake")).toString()
                   == QStringLiteral("Nikon inspected")
            && refreshed_inspection.value(QStringLiteral("orientation")).toInt() == 6,
        "materialization publishes refreshed inspection for the unchanged presentation identity"
    );
    require(
        model.rowCount() == 2 && remoteRow(model) >= 0
            && model.data(model.index(remoteRow(model), 0), ReviewModel::PhotoIdRole).toString()
                   == presentation_photo_id
            && model.data(model.index(remoteRow(model), 0), ReviewModel::IsRemoteRole).toBool()
            && model.data(model.index(remoteRow(model), 0), ReviewModel::RemoteOriginalCachedRole)
                   .toBool(),
        "a cached original must remain a remote-origin row with explicit local residency"
    );
    const QModelIndex retained_remote = model.index(remoteRow(model), 0);
    require(
        model.data(retained_remote, ReviewModel::LocalBackingPhotoIdRole).toString()
                == QStringLiteral("local-materialized-photo")
            && model.data(retained_remote, ReviewModel::VisualAutoTransformRole).toBool()
            && model.data(retained_remote, ReviewModel::OrientationRole).toInt() == 6
            && model.data(retained_remote, ReviewModel::HasCoordinatesRole).toBool(),
        "materialization metadata and local backing identity project without replacing the remote "
        "row"
    );
    require(
        coordinator.statusCode() == QStringLiteral("original-ready-metadata-limited")
            && coordinator.diagnosticText().contains(QStringLiteral("provider unavailable")),
        "inspection degradation remains diagnosable without blocking edit admission"
    );

    QVariantList export_targets;
    export_targets.push_back(
        QVariantMap{
            {QStringLiteral("photoId"), QStringLiteral("local-photo")},
            {QStringLiteral("representationId"), QStringLiteral("local-representation")},
            {QStringLiteral("sourcePath"), QStringLiteral("/local/original.nef")},
            {QStringLiteral("title"), QStringLiteral("Local")},
            {QStringLiteral("isRemote"), false},
        }
    );
    export_targets.push_back(
        QVariantMap{
            {QStringLiteral("photoId"), presentation_photo_id},
            {QStringLiteral("representationId"),
             model.data(retained_remote, ReviewModel::RepresentationIdRole)},
            {QStringLiteral("sourcePath"), QString{}},
            {QStringLiteral("title"), QStringLiteral("Remote A.nef")},
            {QStringLiteral("isRemote"), true},
        }
    );
    QVariantList resolved_export_targets;
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::exportReady,
        [&resolved_export_targets](const QVariantList& targets) {
            resolved_export_targets = targets;
        }
    );
    require(coordinator.prepareExport(export_targets), "mixed remote export preparation admission");
    waitUntil(
        [&resolved_export_targets, &materialize_calls]() {
            return resolved_export_targets.size() == 2
                   && materialize_calls.load(std::memory_order_acquire) == 2;
        },
        "mixed remote export materialization completion"
    );
    const QVariantMap resolved_local = resolved_export_targets.at(0).toMap();
    const QVariantMap resolved_remote = resolved_export_targets.at(1).toMap();
    require(
        resolved_local.value(QStringLiteral("photoId")).toString() == QStringLiteral("local-photo")
            && resolved_remote.value(QStringLiteral("photoId")).toString()
                   == QStringLiteral("local-materialized-photo")
            && resolved_remote.value(QStringLiteral("representationId")).toString()
                   == QStringLiteral("local-materialized-representation")
            && resolved_remote.value(QStringLiteral("sourcePath")).toString() == materialized_source
            && !resolved_remote.value(QStringLiteral("isRemote")).toBool(),
        "mixed export emits only local Catalog-backed execution identities"
    );
    require(
        model.data(model.index(remoteRow(model), 0), ReviewModel::PhotoIdRole).toString()
            == presentation_photo_id,
        "export preparation must not replace the remote presentation identity"
    );
}

void startup_checks_server_without_hiding_a_cached_original() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "offline cached-original settings root");
    const QString settings_file = settings_root.filePath(QStringLiteral("preferences.ini"));
    const QString cached_source = settings_root.filePath(QStringLiteral("cached.nef"));
    QFile cached_file(cached_source);
    require(
        cached_file.open(QIODevice::WriteOnly) && cached_file.write("cached-original") == 15,
        "cached original fixture"
    );
    cached_file.close();

    RemoteLibraryConnectionStore connection_store(settings_file);
    const QString connection_id = connection_store.add(QStringLiteral("offline.local:45321"), true);
    require(!connection_id.isEmpty(), "persisted offline connection identity");
    auto secret_store = makeVolatileSecretStore();
    require(
        secret_store
            ->write(
                QStringLiteral("dev.shadow.photo.remote-library"),
                QStringLiteral("library-sharing-token:") + connection_id,
                QString(32, QLatin1Char('o'))
            )
            .succeeded(),
        "persisted offline connection token"
    );

    BackendRemoteLibrarySnapshot snapshot = remoteSnapshot();
    snapshot.photos.front().has_cached_original = true;
    snapshot.photos.front().local_photo_id = QStringLiteral("cached-local-photo");
    snapshot.photos.front().local_representation_id = QStringLiteral("cached-local-representation");
    snapshot.photos.front().local_source_path = cached_source;
    std::atomic<int> begin_calls = 0;
    std::atomic<int> materialize_calls = 0;
    ReviewModel model;
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [snapshot](const QString&) { return snapshot; },
            .begin_sync = [&begin_calls](const QString&, const QString&, const QString&)
                -> BackendRemoteLibrarySyncStart {
                begin_calls.fetch_add(1, std::memory_order_acq_rel);
                throw std::runtime_error("remote Library request failed: connection refused");
            },
            .sync_step = [](const std::uint64_t) { return BackendRemoteLibrarySyncStep{}; },
            .cancel_sync = [](const std::uint64_t) { return true; },
            .set_review_state = [](const QString&,
                                   const QString&,
                                   const QString&,
                                   BackendReviewDecisionFlag,
                                   std::uint8_t,
                                   bool,
                                   const QString&,
                                   std::int64_t) {},
            .materialize =
                [&materialize_calls, cached_source](
                    const QString&,
                    const QString& address,
                    const QString& token,
                    const QString&,
                    const QString&
                ) {
                    require(
                        address.isEmpty() && token.isEmpty(),
                        "verified cached original must not require a live server"
                    );
                    materialize_calls.fetch_add(1, std::memory_order_acq_rel);
                    return BackendRemoteLibraryMaterialization{
                        .local_photo_id = QStringLiteral("cached-local-photo"),
                        .local_representation_id = QStringLiteral("cached-local-representation"),
                        .local_source_path = cached_source,
                        .title = QStringLiteral("Remote A.nef"),
                        .reused_existing = true,
                        .metadata_schema_version = 1,
                        .camera_make = QStringLiteral("Nikon"),
                    };
                },
        },
        model,
        settings_file,
        std::move(secret_store)
    );

    coordinator.start();
    waitUntil(
        [&coordinator, &begin_calls]() {
            return !coordinator.busy() && coordinator.remotePhotoCount() == 1
                   && begin_calls.load(std::memory_order_acquire) == 1;
        },
        "startup server reachability check"
    );
    const QVariantMap connection = coordinator.connections().front().toMap();
    require(
        coordinator.statusCode() == QStringLiteral("sync-failed") && !coordinator.connected()
            && connection.value(QStringLiteral("reachability")).toString()
                   == QStringLiteral("offline")
            && connection.value(QStringLiteral("mirrorPhotoCount")).toInt() == 1
            && connection.value(QStringLiteral("cachedOriginalCount")).toInt() == 1
            && model.rowCount() == 1
            && model.data(model.index(0, 0), ReviewModel::SourceAvailableRole).toBool()
            && model.data(model.index(0, 0), ReviewModel::RemoteOriginalCachedRole).toBool(),
        "offline reachability must preserve and accurately count the cached original"
    );

    QVariantList prepared_targets;
    QObject::connect(
        &coordinator,
        &ReviewRemoteLibraryCoordinator::exportReady,
        [&prepared_targets](const QVariantList& targets) { prepared_targets = targets; }
    );
    const QModelIndex remote = model.index(0, 0);
    require(
        coordinator.prepareExport({QVariantMap{
            {QStringLiteral("photoId"), model.data(remote, ReviewModel::PhotoIdRole).toString()},
            {QStringLiteral("representationId"),
             model.data(remote, ReviewModel::RepresentationIdRole).toString()},
            {QStringLiteral("sourcePath"), QString{}},
            {QStringLiteral("title"), QStringLiteral("Remote A.nef")},
            {QStringLiteral("isRemote"), true},
        }}),
        "offline cached export admission"
    );
    waitUntil(
        [&prepared_targets, &materialize_calls]() {
            return prepared_targets.size() == 1
                   && materialize_calls.load(std::memory_order_acquire) == 1;
        },
        "offline cached export preparation"
    );
    require(
        prepared_targets.front().toMap().value(QStringLiteral("sourcePath")).toString()
            == cached_source,
        "offline cached export resolves to the Catalog-backed local source"
    );
}

void destruction_without_a_snapshot_future_is_safe() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "empty-future settings root");
    ReviewModel model;
    ReviewRemoteLibraryCoordinator coordinator(
        {},
        model,
        settings_root.filePath(QStringLiteral("preferences.ini")),
        makeVolatileSecretStore()
    );
    require(!coordinator.busy(), "an unstarted coordinator has no snapshot future");
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
                .begin_sync = [](const QString&,
                                 const QString&,
                                 const QString&) { return syncStart(1, remoteSnapshot()); },
                .sync_step = [](
                                 const std::uint64_t job_id
                             ) { return syncComplete(job_id, remoteSnapshot()); },
                .cancel_sync = [](const std::uint64_t) { return true; },
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
            .begin_sync =
                [&sync_calls](
                    const QString& connection_id,
                    const QString& address,
                    const QString& token
                ) {
                    require(!connection_id.isEmpty(), "multi-connection stable identity");
                    require(token.size() == 32, "multi-connection secret routing");
                    const std::uint64_t job_id = static_cast<std::uint64_t>(
                        sync_calls.fetch_add(1, std::memory_order_acq_rel) + 1
                    );
                    return syncStart(
                        job_id,
                        address.startsWith(QStringLiteral("travel")) ? remoteSnapshotB()
                                                                     : remoteSnapshot()
                    );
                },
            .sync_step =
                [](const std::uint64_t job_id) {
                    return syncComplete(job_id, job_id == 2 ? remoteSnapshotB() : remoteSnapshot());
                },
            .cancel_sync = [](const std::uint64_t) { return true; },
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
            && coordinator.remotePhotoCount() == 1 && model.rowCount() == 1
            && coordinator.systemCollectionCounts().value(QStringLiteral("all")).toInt() == 1,
        "removing one remote Library must preserve the other projection"
    );
}

void identical_photo_retries_an_online_alternate_after_download_failure() {
    QTemporaryDir storage;
    ReviewModel model;
    auto first = remoteSnapshot();
    auto second = remoteSnapshotB();
    for (auto* snapshot : {&first, &second}) {
        snapshot->photos.front().has_original_identity = true;
        snapshot->photos.front().original_digest_hex = QString(64, QLatin1Char('d'));
    }
    std::atomic<int> sync_calls{0};
    std::atomic<int> download_calls{0};
    int ready_count = 0;
    const QString path = storage.filePath(QStringLiteral("materialized.nef"));
    QFile local(path);
    require(
        local.open(QIODevice::WriteOnly) && local.write("original") == 8,
        "alternate original fixture"
    );
    local.close();
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
            .begin_sync =
                [&](const QString&, const QString& address, const QString&) {
                    const auto job = static_cast<std::uint64_t>(++sync_calls);
                    return syncStart(
                        job,
                        address.startsWith(QStringLiteral("first")) ? first : second
                    );
                },
            .sync_step = [&](
                             const std::uint64_t job
                         ) { return syncComplete(job, job == 1 ? first : second); },
            .cancel_sync = [](const std::uint64_t) { return true; },
            .set_review_state = [](const QString&,
                                   const QString&,
                                   const QString&,
                                   BackendReviewDecisionFlag,
                                   std::uint8_t,
                                   bool,
                                   const QString&,
                                   std::int64_t) {},
            .materialize =
                [&](const QString&,
                    const QString&,
                    const QString&,
                    const QString&,
                    const QString&) {
                    if (++download_calls == 1)
                        throw std::runtime_error("connection reset by peer");
                    BackendRemoteLibraryMaterialization result;
                    result.local_photo_id = QStringLiteral("downloaded-photo");
                    result.local_representation_id = QStringLiteral("downloaded-representation");
                    result.local_source_path = path;
                    return result;
                },
        },
        model,
        storage.filePath(QStringLiteral("settings.ini")),
        makeVolatileSecretStore()
    );
    QObject::connect(&coordinator, &ReviewRemoteLibraryCoordinator::remotePhotoReady, [&]() {
        ++ready_count;
    });
    require(
        !coordinator
             .saveConnection({}, QStringLiteral("first.local:45321"), QString(32, QLatin1Char('x')))
             .isEmpty(),
        "first origin"
    );
    waitUntil([&]() { return !coordinator.busy(); }, "first origin sync");
    require(
        !coordinator
             .saveConnection(
                 {},
                 QStringLiteral("second.local:45321"),
                 QString(32, QLatin1Char('y'))
             )
             .isEmpty(),
        "second origin"
    );
    waitUntil([&]() { return !coordinator.busy(); }, "second origin sync");
    require(model.rowCount() == 1, "identical originals retain one presentation");
    const QString id = model.data(model.index(0, 0), ReviewModel::PhotoIdRole).toString();
    coordinator.materializeForEdit(id);
    waitUntil([&]() { return !coordinator.materializing(); }, "alternate materialization");
    require(
        download_calls == 2 && ready_count == 1,
        "failed download retries another online original exactly once"
    );
    require(
        model.data(model.index(0, 0), ReviewModel::PhotoIdRole).toString() == id,
        "fallback preserves the logical photo identity"
    );
}

void progressive_sync_publishes_each_page_before_the_next_page_finishes() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "progressive page settings root");
    ReviewModel model;
    std::atomic<int> step_calls = 0;
    std::atomic<bool> second_page_entered = false;
    std::atomic<bool> release_second_page = false;
    const BackendRemoteLibrarySnapshot first_page = remoteSnapshotNamed(
        QStringLiteral("page-photo-a"),
        QStringLiteral("page-representation-a"),
        QStringLiteral("Page A.nef")
    );
    const BackendRemoteLibrarySnapshot complete = twoPhotoSnapshot();
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
            .begin_sync = [](const QString&,
                             const QString&,
                             const QString&) { return syncStart(41, emptyRemoteSnapshot()); },
            .sync_step =
                [first_page, complete, &step_calls, &second_page_entered, &release_second_page](
                    const std::uint64_t job_id
                ) {
                    const int call = step_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (call == 1) {
                        return BackendRemoteLibrarySyncStep{
                            .job_id = job_id,
                            .snapshot = first_page,
                            .stage = QStringLiteral("manifest"),
                            .page_count = 1,
                            .photo_count = 1,
                            .manifest_complete = false,
                            .complete = false,
                        };
                    }
                    second_page_entered.store(true, std::memory_order_release);
                    while (!release_second_page.load(std::memory_order_acquire)) {
                        QThread::msleep(1);
                    }
                    return BackendRemoteLibrarySyncStep{
                        .job_id = job_id,
                        .snapshot = complete,
                        .stage = QStringLiteral("manifest"),
                        .page_count = 2,
                        .photo_count = 2,
                        .manifest_complete = true,
                        .complete = true,
                    };
                },
            .cancel_sync = [](const std::uint64_t) { return true; },
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
    const QString connection_id = coordinator.saveConnection(
        {},
        QStringLiteral("pages.local:45321"),
        QString(32, QLatin1Char('p'))
    );
    require(!connection_id.isEmpty(), "progressive connection admission");
    waitUntil(
        [&model, &second_page_entered]() {
            return model.rowCount() == 1 && second_page_entered.load(std::memory_order_acquire);
        },
        "first manifest page visible while second page is blocked"
    );
    require(
        model.data(model.index(0, 0), ReviewModel::TitleRole).toString()
            == QStringLiteral("Page A.nef"),
        "first page enters ReviewModel immediately"
    );
    const QVariantMap progress = coordinator.connections().front().toMap();
    require(
        progress.value(QStringLiteral("syncPageCount")).toULongLong() == 1
            && progress.value(QStringLiteral("syncPhotoCount")).toULongLong() == 1,
        "connection projection exposes page and photo progress"
    );
    release_second_page.store(true, std::memory_order_release);
    waitUntil(
        [&coordinator, &model]() { return !coordinator.busy() && model.rowCount() == 2; },
        "second page completion"
    );
}

void preview_failure_keeps_the_manifest_photo_visible() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "preview failure settings root");
    ReviewModel model;
    BackendRemoteLibrarySnapshot manifest_only = remoteSnapshot();
    manifest_only.photos.front().has_preview = false;
    manifest_only.photos.front().preview_path.clear();
    manifest_only.photos.front().preview_unavailable_reason =
        QStringLiteral("preview_cache_unavailable");
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
            .begin_sync = [](const QString&,
                             const QString&,
                             const QString&) { return syncStart(51, emptyRemoteSnapshot()); },
            .sync_step = [manifest_only](
                             const std::uint64_t job_id
                         ) { return syncComplete(job_id, manifest_only, 1); },
            .cancel_sync = [](const std::uint64_t) { return true; },
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
    const QString connection_id = coordinator.saveConnection(
        {},
        QStringLiteral("previews.local:45321"),
        QString(32, QLatin1Char('v'))
    );
    require(!connection_id.isEmpty(), "preview failure connection admission");
    waitUntil([&coordinator]() { return !coordinator.busy(); }, "preview failure completion");
    require(
        model.rowCount() == 1
            && model.data(model.index(0, 0), ReviewModel::VisualSourceRole).toString().isEmpty()
            && coordinator.statusCode() == QStringLiteral("synchronized-preview-limited")
            && coordinator.diagnosticText().contains(QStringLiteral("preview unavailable")),
        "preview failure remains a visible metadata row with a diagnostic"
    );
}

void stale_sync_job_cannot_overwrite_a_newer_request() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "stale sync settings root");
    ReviewModel model;
    std::atomic<int> begin_calls = 0;
    std::atomic<bool> first_begin_entered = false;
    std::atomic<bool> release_first_begin = false;
    std::atomic<int> cancelled_job = 0;
    const BackendRemoteLibrarySnapshot stale_snapshot = remoteSnapshotNamed(
        QStringLiteral("stale-photo"),
        QStringLiteral("stale-representation"),
        QStringLiteral("Stale.nef")
    );
    const BackendRemoteLibrarySnapshot current_snapshot = remoteSnapshotNamed(
        QStringLiteral("current-photo"),
        QStringLiteral("current-representation"),
        QStringLiteral("Current.nef")
    );
    ReviewRemoteLibraryCoordinator coordinator(
        {
            .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
            .begin_sync =
                [stale_snapshot,
                 current_snapshot,
                 &begin_calls,
                 &first_begin_entered,
                 &release_first_begin](const QString&, const QString&, const QString&) {
                    const int call = begin_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (call == 1) {
                        first_begin_entered.store(true, std::memory_order_release);
                        while (!release_first_begin.load(std::memory_order_acquire)) {
                            QThread::msleep(1);
                        }
                        return syncStart(61, stale_snapshot);
                    }
                    return syncStart(62, current_snapshot);
                },
            .sync_step = [current_snapshot](
                             const std::uint64_t job_id
                         ) { return syncComplete(job_id, current_snapshot); },
            .cancel_sync =
                [&cancelled_job](const std::uint64_t job_id) {
                    cancelled_job.store(static_cast<int>(job_id), std::memory_order_release);
                    return true;
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
    const QString connection_id = coordinator.saveConnection(
        {},
        QStringLiteral("stale.local:45321"),
        QString(32, QLatin1Char('s'))
    );
    require(!connection_id.isEmpty(), "stale job connection admission");
    waitUntil(
        [&first_begin_entered]() { return first_begin_entered.load(std::memory_order_acquire); },
        "first begin enters worker"
    );
    coordinator.syncNow(connection_id);
    release_first_begin.store(true, std::memory_order_release);
    waitUntil(
        [&coordinator, &begin_calls]() {
            return !coordinator.busy() && begin_calls.load(std::memory_order_acquire) == 2;
        },
        "replacement sync completion"
    );
    require(
        cancelled_job.load(std::memory_order_acquire) == 61 && model.rowCount() == 1
            && model.data(model.index(0, 0), ReviewModel::TitleRole).toString()
                   == QStringLiteral("Current.nef"),
        "stale begin result is cancelled and never projected"
    );
}

void destruction_cancels_an_incomplete_progressive_job() {
    QTemporaryDir settings_root;
    require(settings_root.isValid(), "destructor cancellation settings root");
    ReviewModel model;
    std::atomic<bool> step_entered = false;
    std::atomic<bool> release_step = false;
    std::atomic<int> cancellation_count = 0;
    std::thread releaser;
    {
        ReviewRemoteLibraryCoordinator coordinator(
            {
                .snapshot = [](const QString&) { return BackendRemoteLibrarySnapshot{}; },
                .begin_sync = [](const QString&,
                                 const QString&,
                                 const QString&) { return syncStart(71, emptyRemoteSnapshot()); },
                .sync_step =
                    [&step_entered, &release_step](const std::uint64_t job_id) {
                        step_entered.store(true, std::memory_order_release);
                        while (!release_step.load(std::memory_order_acquire)) {
                            QThread::msleep(1);
                        }
                        return BackendRemoteLibrarySyncStep{
                            .job_id = job_id,
                            .snapshot = emptyRemoteSnapshot(),
                            .stage = QStringLiteral("manifest"),
                        };
                    },
                .cancel_sync =
                    [&cancellation_count](const std::uint64_t) {
                        cancellation_count.fetch_add(1, std::memory_order_acq_rel);
                        return true;
                    },
                .set_review_state = [](const QString&,
                                       const QString&,
                                       const QString&,
                                       BackendReviewDecisionFlag,
                                       std::uint8_t,
                                       bool,
                                       const QString&,
                                       std::int64_t) {},
                .materialize = [](const QString&,
                                  const QString&,
                                  const QString&,
                                  const QString&,
                                  const QString&) { return BackendRemoteLibraryMaterialization{}; },
            },
            model,
            settings_root.filePath(QStringLiteral("preferences.ini")),
            makeVolatileSecretStore()
        );
        require(
            !coordinator
                 .saveConnection(
                     {},
                     QStringLiteral("cancel.local:45321"),
                     QString(32, QLatin1Char('c'))
                 )
                 .isEmpty(),
            "destructor cancellation connection admission"
        );
        waitUntil(
            [&step_entered]() { return step_entered.load(std::memory_order_acquire); },
            "sync step enters worker before destruction"
        );
        releaser = std::thread([&release_step]() {
            QThread::msleep(20);
            release_step.store(true, std::memory_order_release);
        });
    }
    releaser.join();
    require(
        cancellation_count.load(std::memory_order_acquire) >= 1,
        "coordinator destruction cancels the retained backend job"
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
    const auto aggregate =
        aggregates.constFind(QStringLiteral("remote-content:") + QString(64, QLatin1Char('a')));
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
    const auto cached_aggregate =
        cached.constFind(QStringLiteral("remote-content:") + QString(64, QLatin1Char('a')));
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

    studio.photos.front().has_cached_original = false;
    studio.server.originals_available = true;
    const auto reachable = aggregateRemotePhotos(
        {
            {QStringLiteral("connection-studio"), studio},
            {QStringLiteral("connection-travel"), travel},
        },
        QSet<QString>{QStringLiteral("connection-travel")}
    );
    require(
        reachable.cbegin()->preferredSource()->connection_id == QStringLiteral("connection-travel"),
        "an online identical original wins over a capable but offline server"
    );

    studio.photos.front().has_original_identity = false;
    studio.photos.front().original_digest_hex.clear();
    travel.photos.front().has_original_identity = false;
    travel.photos.front().original_digest_hex.clear();
    require(
        aggregateRemotePhotos({
                                  {QStringLiteral("connection-studio"), studio},
                                  {QStringLiteral("connection-travel"), travel},
                              })
                .size()
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
            .begin_sync = [](const QString&, const QString&, const QString&)
                -> BackendRemoteLibrarySyncStart {
                throw std::runtime_error("remote Library request failed: authorization failed");
            },
            .sync_step = [](const std::uint64_t) { return BackendRemoteLibrarySyncStep{}; },
            .cancel_sync = [](const std::uint64_t) { return true; },
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
    startup_checks_server_without_hiding_a_cached_original();
    shutdown_drains_queued_remote_curation();
    multiple_connections_keep_independent_identity_and_projection();
    identical_photo_retries_an_online_alternate_after_download_failure();
    progressive_sync_publishes_each_page_before_the_next_page_finishes();
    preview_failure_keeps_the_manifest_photo_visible();
    stale_sync_job_cannot_overwrite_a_newer_request();
    destruction_cancels_an_incomplete_progressive_job();
    destruction_without_a_snapshot_future_is_safe();
    exact_original_identity_merges_server_copies_and_retains_sources();
    connection_store_preserves_stable_ids_and_legacy_migration();
    authorization_rejection_is_actionable();
    return EXIT_SUCCESS;
}

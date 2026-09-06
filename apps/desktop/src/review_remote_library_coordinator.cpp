#include "review_remote_library_coordinator.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QTimeZone>
#include <QUrl>
#include <QVariantMap>
#include <QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace {

constexpr auto secret_service = "dev.shadow.photo.remote-library";
constexpr auto legacy_secret_account = "library-sharing-token";
constexpr auto secret_account_prefix = "library-sharing-token:";

[[nodiscard]] bool validAddress(const QString& value) {
    const QString normalized = value.trimmed();
    return !normalized.isEmpty() && normalized.size() <= 512
           && std::none_of(normalized.cbegin(), normalized.cend(), [](const QChar character) {
                  return character.isSpace() || !character.isPrint();
              });
}

[[nodiscard]] bool validToken(const QString& value) {
    const QString normalized = value.trimmed();
    return normalized.size() >= 32 && normalized.size() <= 4'096
           && std::all_of(normalized.cbegin(), normalized.cend(), [](const QChar character) {
                  return character.isPrint() && !character.isSpace();
              });
}

[[nodiscard]] bool transportUnavailable(const QString& diagnostic) {
    for (const auto* marker :
         {"offline",
          "connection refused",
          "connect error",
          "error sending request",
          "timed out",
          "timeout",
          "unreachable",
          "dns",
          "network"}) {
        if (diagnostic.contains(QLatin1String(marker), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

[[nodiscard]] QString syncFailureStatus(const QString& diagnostic) {
    if (diagnostic.contains(QStringLiteral("authorization failed"), Qt::CaseInsensitive)
        || diagnostic.contains(QStringLiteral("unauthorized"), Qt::CaseInsensitive)) {
        return QStringLiteral("authorization-failed");
    }
    if (diagnostic.contains(QStringLiteral("server is busy"), Qt::CaseInsensitive)) {
        return QStringLiteral("server-busy");
    }
    return QStringLiteral("sync-failed");
}

[[nodiscard]] QString decisionFlagName(const BackendReviewDecisionFlag flag) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return QStringLiteral("unflagged");
    case BackendReviewDecisionFlag::Picked:
        return QStringLiteral("picked");
    case BackendReviewDecisionFlag::Rejected:
        return QStringLiteral("rejected");
    }
    return QStringLiteral("unflagged");
}

[[nodiscard]] bool validColorLabel(const QString& value) {
    return value == QStringLiteral("none") || value == QStringLiteral("red")
           || value == QStringLiteral("yellow") || value == QStringLiteral("green")
           || value == QStringLiteral("blue") || value == QStringLiteral("purple");
}

[[nodiscard]] int boundedPhotoCount(const qsizetype count) {
    return static_cast<int>(std::min<qsizetype>(count, std::numeric_limits<int>::max()));
}

[[nodiscard]] bool cachedOriginalAvailable(const BackendRemoteLibraryPhoto& photo) {
    return photo.has_cached_original && !photo.local_photo_id.isEmpty()
           && !photo.local_representation_id.isEmpty()
           && QFileInfo(photo.local_source_path).isFile()
           && QFileInfo(photo.local_source_path).isReadable();
}

[[nodiscard]] QString
reachabilityStatus(const QString& status_code, const bool online, const bool offline) {
    if (online) {
        return QStringLiteral("online");
    }
    if (status_code == QStringLiteral("synchronizing")) {
        return QStringLiteral("checking");
    }
    if (status_code == QStringLiteral("authorization-failed")) {
        return QStringLiteral("authorization-failed");
    }
    if (offline) {
        return QStringLiteral("offline");
    }
    return QStringLiteral("unknown");
}

void applyMaterializationMetadata(
    BackendRemoteLibraryPhoto& photo,
    const BackendRemoteLibraryMaterialization& materialization
) {
    photo.metadata_schema_version = materialization.metadata_schema_version;
    photo.has_captured_at = materialization.has_captured_at;
    photo.captured_at_unix_seconds = materialization.captured_at_unix_seconds;
    photo.camera_make = materialization.camera_make;
    photo.camera_model = materialization.camera_model;
    photo.lens_make = materialization.lens_make;
    photo.lens_model = materialization.lens_model;
    photo.has_iso_speed = materialization.has_iso_speed;
    photo.iso_speed = materialization.iso_speed;
    photo.has_exposure_time = materialization.has_exposure_time;
    photo.exposure_time_seconds = materialization.exposure_time_seconds;
    photo.has_aperture = materialization.has_aperture;
    photo.aperture_f_number = materialization.aperture_f_number;
    photo.has_focal_length = materialization.has_focal_length;
    photo.focal_length_mm = materialization.focal_length_mm;
    photo.has_focal_length_35mm = materialization.has_focal_length_35mm;
    photo.focal_length_35mm = materialization.focal_length_35mm;
    photo.has_raw_dimensions = materialization.has_raw_dimensions;
    photo.raw_width = materialization.raw_width;
    photo.raw_height = materialization.raw_height;
    photo.has_image_dimensions = materialization.has_image_dimensions;
    photo.image_width = materialization.image_width;
    photo.image_height = materialization.image_height;
    photo.has_orientation = materialization.has_orientation;
    photo.orientation = materialization.orientation;
    photo.has_coordinates = materialization.has_coordinates;
    photo.latitude_degrees = materialization.latitude_degrees;
    photo.longitude_degrees = materialization.longitude_degrees;
    photo.has_altitude = materialization.has_altitude;
    photo.altitude_meters = materialization.altitude_meters;
}

[[nodiscard]] QVariantMap remoteInspection(
    const QString& presentation_photo_id,
    const QString& presentation_representation_id,
    const BackendRemoteLibraryPhoto& photo
) {
    const bool has_metadata =
        photo.has_captured_at || !photo.camera_make.isEmpty() || !photo.camera_model.isEmpty()
        || !photo.lens_make.isEmpty() || !photo.lens_model.isEmpty() || photo.has_iso_speed
        || photo.has_exposure_time || photo.has_aperture || photo.has_focal_length
        || photo.has_focal_length_35mm || photo.has_raw_dimensions || photo.has_image_dimensions
        || photo.has_orientation || photo.has_coordinates;
    return {
        {QStringLiteral("available"), true},
        {QStringLiteral("photoId"), presentation_photo_id},
        {QStringLiteral("representationId"), presentation_representation_id},
        {QStringLiteral("hasMetadata"), has_metadata},
        {QStringLiteral("cameraMake"), photo.camera_make},
        {QStringLiteral("cameraModel"), photo.camera_model},
        {QStringLiteral("lensMake"), photo.lens_make},
        {QStringLiteral("lensModel"), photo.lens_model},
        {QStringLiteral("capturedAtUnixSeconds"), photo.captured_at_unix_seconds},
        {QStringLiteral("hasCoordinates"), photo.has_coordinates},
        {QStringLiteral("latitude"), photo.latitude_degrees},
        {QStringLiteral("longitude"), photo.longitude_degrees},
        {QStringLiteral("hasAltitude"), photo.has_altitude},
        {QStringLiteral("altitudeMeters"), photo.altitude_meters},
        {QStringLiteral("isoSpeed"), photo.iso_speed},
        {QStringLiteral("exposureTimeSeconds"), photo.exposure_time_seconds},
        {QStringLiteral("apertureFNumber"), photo.aperture_f_number},
        {QStringLiteral("focalLengthMm"), photo.focal_length_mm},
        {QStringLiteral("focalLength35mm"), photo.focal_length_35mm},
        {QStringLiteral("rawWidth"), photo.raw_width},
        {QStringLiteral("rawHeight"), photo.raw_height},
        {QStringLiteral("imageWidth"), photo.image_width},
        {QStringLiteral("imageHeight"), photo.image_height},
        {QStringLiteral("hasOrientation"), photo.has_orientation},
        {QStringLiteral("orientation"), photo.orientation},
    };
}

} // namespace

ReviewRemoteLibraryCoordinator::ReviewRemoteLibraryCoordinator(
    Operations operations,
    ReviewModel& model,
    const QString& isolated_settings_file,
    std::unique_ptr<SecretStore> secret_store,
    QObject* const parent
) :
    QObject(parent), operations_(std::move(operations)), model_(&model),
    connection_store_(isolated_settings_file), secret_store_(std::move(secret_store)) {
    connect(
        &snapshot_watcher_,
        &QFutureWatcher<SnapshotTaskResult>::finished,
        this,
        &ReviewRemoteLibraryCoordinator::finishSnapshotTask
    );
    connect(
        &materialize_watcher_,
        &QFutureWatcher<MaterializeTaskResult>::finished,
        this,
        &ReviewRemoteLibraryCoordinator::finishMaterializeTask
    );
    connect(
        &mutation_watcher_,
        &QFutureWatcher<MutationTaskResult>::finished,
        this,
        &ReviewRemoteLibraryCoordinator::finishMutationTask
    );
    reachability_refresh_timer_.setInterval(60'000);
    reachability_refresh_timer_.setTimerType(Qt::VeryCoarseTimer);
    connect(&reachability_refresh_timer_, &QTimer::timeout, this, [this]() {
        if (!busy() && !mutation_task_active_)
            syncAll();
    });
    migrateLegacySecret();
}

ReviewRemoteLibraryCoordinator::~ReviewRemoteLibraryCoordinator() {
    snapshot_queue_.clear();
    for (auto job = active_sync_job_ids_.cbegin(); job != active_sync_job_ids_.cend(); ++job) {
        try {
            (void)operations_.cancel_sync(job.value());
        } catch (...) {}
    }
    QFuture<SnapshotTaskResult> snapshot_future = snapshot_watcher_.future();
    if (snapshot_future.isValid()) {
        snapshot_future.waitForFinished();
    }
    if (snapshot_future.isValid() && snapshot_future.resultCount() > 0) {
        const SnapshotTaskResult result = snapshot_future.resultAt(0);
        const std::uint64_t returned_job_id =
            result.kind == SnapshotTaskKind::BeginSync  ? result.sync_start.job_id
            : result.kind == SnapshotTaskKind::SyncStep ? result.sync_step.job_id
                                                        : 0;
        if (returned_job_id != 0) {
            try {
                (void)operations_.cancel_sync(returned_job_id);
            } catch (...) {}
        }
    }
    materialize_watcher_.waitForFinished();
    mutation_watcher_.waitForFinished();
    while (!mutation_queue_.isEmpty()) {
        (void)runMutationTask(operations_, mutation_queue_.dequeue());
    }
}

void ReviewRemoteLibraryCoordinator::start() {
    if (started_) {
        return;
    }
    started_ = true;
    reachability_refresh_timer_.start();
    for (const auto& connection : connection_store_.connections()) {
        snapshot_queue_.enqueue({
            .kind = SnapshotTaskKind::LoadCached,
            .connection_id = connection.id,
        });
    }
    if (snapshot_queue_.isEmpty()) {
        setStatus(QStringLiteral("offline-ready"));
    }
    startNextSnapshotTask();
    emit stateChanged();
}

bool ReviewRemoteLibraryCoordinator::busy() const noexcept {
    return !active_snapshot_connection_id_.isEmpty() || !snapshot_queue_.isEmpty()
           || !materializing_connection_id_.isEmpty();
}

bool ReviewRemoteLibraryCoordinator::syncing() const noexcept {
    return !active_snapshot_connection_id_.isEmpty() || !snapshot_queue_.isEmpty();
}

bool ReviewRemoteLibraryCoordinator::materializing() const noexcept {
    return !materializing_connection_id_.isEmpty();
}

bool ReviewRemoteLibraryCoordinator::secureStorageAvailable() const noexcept {
    return secret_store_ != nullptr && secret_store_->available();
}

bool ReviewRemoteLibraryCoordinator::tokenStored() const noexcept {
    return std::any_of(
        connection_store_.connections().cbegin(),
        connection_store_.connections().cend(),
        [](const RemoteLibraryConnection& connection) { return connection.token_stored; }
    );
}

bool ReviewRemoteLibraryCoordinator::connected() const noexcept {
    return !online_connection_ids_.isEmpty();
}

QVariantList ReviewRemoteLibraryCoordinator::connections() const {
    QVariantList result;
    result.reserve(connection_store_.connections().size());
    for (const auto& connection : connection_store_.connections()) {
        const auto snapshot = snapshots_.constFind(connection.id);
        const bool has_server = snapshot != snapshots_.cend() && snapshot->has_server;
        const SyncProgress progress = sync_progress_.value(connection.id);
        const int mirror_photo_count =
            snapshot == snapshots_.cend() ? 0 : boundedPhotoCount(snapshot->photos.size());
        const int cached_original_count = snapshot == snapshots_.cend()
                                              ? 0
                                              : boundedPhotoCount(
                                                    std::count_if(
                                                        snapshot->photos.cbegin(),
                                                        snapshot->photos.cend(),
                                                        cachedOriginalAvailable
                                                    )
                                                );
        const QString status_code = connection_status_codes_.value(connection.id);
        const bool online = online_connection_ids_.contains(connection.id);
        result.push_back(
            QVariantMap{
                {QStringLiteral("id"), connection.id},
                {QStringLiteral("address"), connection.address},
                {QStringLiteral("tokenStored"), connection.token_stored},
                {QStringLiteral("configured"),
                 connection.token_stored && !connection.address.isEmpty()},
                {QStringLiteral("connected"), online},
                {QStringLiteral("reachability"),
                 reachabilityStatus(
                     status_code,
                     online,
                     offline_connection_ids_.contains(connection.id)
                 )},
                {QStringLiteral("serverName"),
                 has_server ? snapshot->server.display_name : QString{}},
                {QStringLiteral("hasCachedServer"), has_server},
                {QStringLiteral("originalsAvailable"),
                 has_server && snapshot->server.originals_available},
                {QStringLiteral("photoCount"), mirror_photo_count},
                {QStringLiteral("mirrorPhotoCount"), mirror_photo_count},
                {QStringLiteral("cachedOriginalCount"), cached_original_count},
                {QStringLiteral("statusCode"), status_code},
                {QStringLiteral("diagnosticText"), connection_diagnostics_.value(connection.id)},
                {QStringLiteral("syncPageCount"), QVariant::fromValue(progress.page_count)},
                {QStringLiteral("syncPhotoCount"), QVariant::fromValue(progress.photo_count)},
                {QStringLiteral("syncPreviewCompletedCount"),
                 QVariant::fromValue(progress.preview_completed_count)},
                {QStringLiteral("syncDownloadedPreviewCount"),
                 QVariant::fromValue(progress.downloaded_previews)},
                {QStringLiteral("syncPreviewFailureCount"),
                 QVariant::fromValue(progress.preview_failures)},
                {QStringLiteral("syncRemovedCount"), QVariant::fromValue(progress.removed)},
                {QStringLiteral("syncManifestComplete"), progress.manifest_complete},
                {QStringLiteral("busy"),
                 active_snapshot_connection_id_ == connection.id
                     || materializing_connection_id_ == connection.id},
            }
        );
    }
    return result;
}

QString ReviewRemoteLibraryCoordinator::serverAddress() const {
    return connection_store_.connections().isEmpty()
               ? QString{}
               : connection_store_.connections().front().address;
}

bool ReviewRemoteLibraryCoordinator::hasServer() const noexcept {
    return std::any_of(snapshots_.cbegin(), snapshots_.cend(), [](const auto& snapshot) {
        return snapshot.has_server;
    });
}

QString ReviewRemoteLibraryCoordinator::serverName() const {
    for (const auto& connection : connection_store_.connections()) {
        const auto snapshot = snapshots_.constFind(connection.id);
        if (snapshot != snapshots_.cend() && snapshot->has_server) {
            return snapshot->server.display_name;
        }
    }
    return {};
}

int ReviewRemoteLibraryCoordinator::remotePhotoCount() const noexcept {
    return boundedPhotoCount(photos_.size());
}

QVariantMap ReviewRemoteLibraryCoordinator::systemCollectionCounts() const {
    qulonglong liked = 0;
    qulonglong five_star = 0;
    // Count the logical projection, after cross-server identity merging and
    // optimistic curation, independently of the currently visible Library scope.
    for (const auto& photo : photos_) {
        liked += photo.liked ? 1U : 0U;
        five_star += photo.decision_rating == 5 ? 1U : 0U;
    }
    return {
        {QStringLiteral("all"), QVariant::fromValue(static_cast<qulonglong>(photos_.size()))},
        {QStringLiteral("liked"), QVariant::fromValue(liked)},
        {QStringLiteral("fiveStar"), QVariant::fromValue(five_star)},
    };
}

QString ReviewRemoteLibraryCoordinator::statusCode() const {
    return status_code_;
}

QString ReviewRemoteLibraryCoordinator::diagnosticText() const {
    return diagnostic_text_;
}

QString ReviewRemoteLibraryCoordinator::materializingPhotoId() const {
    return materializing_photo_id_;
}

bool ReviewRemoteLibraryCoordinator::ownsPresentationPhoto(
    const QString& presentation_photo_id
) const {
    return photos_.contains(presentation_photo_id);
}

QString ReviewRemoteLibraryCoordinator::saveConnection(
    const QString& connection_id,
    const QString& server_address,
    const QString& token
) {
    if (busy()) {
        setStatus(QStringLiteral("operation-busy"));
        return {};
    }
    const QString normalized_address = server_address.trimmed();
    const QString normalized_token = token.trimmed();
    if (!validAddress(normalized_address)) {
        setStatus(QStringLiteral("invalid-address"));
        return {};
    }
    const RemoteLibraryConnection* existing = connection(connection_id);
    if ((existing == nullptr || !existing->token_stored) && !validToken(normalized_token)) {
        setStatus(QStringLiteral("invalid-token"));
        return {};
    }
    if (!normalized_token.isEmpty() && !validToken(normalized_token)) {
        setStatus(QStringLiteral("invalid-token"));
        return {};
    }
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return {};
    }
    for (const auto& candidate : connection_store_.connections()) {
        if (candidate.id != connection_id && candidate.address == normalized_address) {
            setStatus(QStringLiteral("duplicate-address"));
            return {};
        }
    }

    QString resolved_id = connection_id;
    bool token_stored = existing != nullptr && existing->token_stored;
    if (existing == nullptr) {
        resolved_id = connection_store_.add(normalized_address, false);
    }
    const RemoteLibraryConnection* resolved = connection(resolved_id);
    if (resolved == nullptr) {
        setStatus(QStringLiteral("connection-save-failed"));
        return {};
    }
    if (!normalized_token.isEmpty()) {
        const SecretStoreResult stored = secret_store_->write(
            QString::fromLatin1(secret_service),
            QString::fromLatin1(secret_account_prefix) + resolved_id,
            normalized_token
        );
        if (!stored.succeeded()) {
            if (existing == nullptr) {
                (void)connection_store_.remove(resolved_id);
            }
            setStatus(QStringLiteral("secret-store-failed"), stored.diagnostic);
            return {};
        }
        token_stored = true;
        if (resolved->uses_legacy_secret) {
            (void)connection_store_.finishLegacySecretMigration(resolved_id);
        }
    }
    if (!connection_store_.update(resolved_id, normalized_address, token_stored)) {
        setStatus(QStringLiteral("connection-save-failed"));
        return {};
    }
    setConnectionStatus(resolved_id, QStringLiteral("connection-saved"));
    emit connectionChanged();
    emit stateChanged();
    syncNow(resolved_id);
    return resolved_id;
}

bool ReviewRemoteLibraryCoordinator::saveConnection(
    const QString& server_address,
    const QString& token
) {
    const QString existing_id = connection_store_.connections().isEmpty()
                                    ? QString{}
                                    : connection_store_.connections().front().id;
    return !saveConnection(existing_id, server_address, token).isEmpty();
}

bool ReviewRemoteLibraryCoordinator::removeConnection(const QString& connection_id) {
    if (busy()) {
        setStatus(QStringLiteral("operation-busy"));
        return false;
    }
    const RemoteLibraryConnection* existing = connection(connection_id);
    if (existing == nullptr) {
        return false;
    }
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult removed =
        secret_store_->remove(QString::fromLatin1(secret_service), tokenAccount(*existing));
    if (removed.status != SecretStoreStatus::Success
        && removed.status != SecretStoreStatus::NotFound) {
        setStatus(QStringLiteral("secret-store-failed"), removed.diagnostic);
        return false;
    }
    if (existing->uses_legacy_secret) {
        (void)secret_store_->remove(
            QString::fromLatin1(secret_service),
            QString::fromLatin1(legacy_secret_account)
        );
    }
    if (!connection_store_.remove(connection_id)) {
        return false;
    }
    snapshots_.remove(connection_id);
    connection_status_codes_.remove(connection_id);
    connection_diagnostics_.remove(connection_id);
    sync_progress_.remove(connection_id);
    active_sync_job_ids_.remove(connection_id);
    latest_sync_epochs_.remove(connection_id);
    online_connection_ids_.remove(connection_id);
    offline_connection_ids_.remove(connection_id);
    rebuildPhotoAggregates();
    reapplyRemoteItems();
    setStatus(QStringLiteral("connection-removed"));
    emit connectionChanged();
    emit stateChanged();
    return true;
}

bool ReviewRemoteLibraryCoordinator::removeConnection() {
    return !connection_store_.connections().isEmpty()
           && removeConnection(connection_store_.connections().front().id);
}

void ReviewRemoteLibraryCoordinator::syncNow(const QString& connection_id) {
    if (materialize_watcher_.isRunning()) {
        setConnectionStatus(connection_id, QStringLiteral("operation-busy"));
        return;
    }
    const RemoteLibraryConnection* requested = connection(connection_id);
    if (requested == nullptr || !validAddress(requested->address) || !requested->token_stored) {
        setConnectionStatus(connection_id, QStringLiteral("connection-required"));
        return;
    }
    const SecretStoreResult authorization = readAuthorization(connection_id);
    if (!authorization.succeeded() || !validToken(authorization.value)) {
        if (authorization.status == SecretStoreStatus::NotFound) {
            (void)connection_store_.setTokenStored(connection_id, false);
            emit connectionChanged();
        }
        setConnectionStatus(
            connection_id,
            QStringLiteral("token-required"),
            authorization.diagnostic
        );
        return;
    }
    next_sync_epoch_ =
        next_sync_epoch_ == std::numeric_limits<std::uint64_t>::max() ? 1 : next_sync_epoch_ + 1;
    const std::uint64_t sync_epoch = next_sync_epoch_;
    latest_sync_epochs_.insert(connection_id, sync_epoch);
    QQueue<SnapshotRequest> retained_requests;
    while (!snapshot_queue_.isEmpty()) {
        SnapshotRequest queued = snapshot_queue_.dequeue();
        if (queued.connection_id == connection_id && queued.kind != SnapshotTaskKind::LoadCached) {
            if (queued.job_id != 0) {
                try {
                    (void)operations_.cancel_sync(queued.job_id);
                } catch (...) {}
            }
            continue;
        }
        retained_requests.enqueue(std::move(queued));
    }
    snapshot_queue_ = std::move(retained_requests);
    if (active_snapshot_connection_id_ != connection_id) {
        const std::uint64_t active_job_id = active_sync_job_ids_.take(connection_id);
        if (active_job_id != 0) {
            try {
                (void)operations_.cancel_sync(active_job_id);
            } catch (...) {}
        }
    }
    sync_progress_.insert(connection_id, SyncProgress{});
    const bool availability_changed = online_connection_ids_.remove(connection_id);
    setConnectionStatus(connection_id, QStringLiteral("synchronizing"));
    snapshot_queue_.enqueue({
        .kind = SnapshotTaskKind::BeginSync,
        .connection_id = connection_id,
        .server_address = requested->address,
        .authorization = authorization.value,
        .sync_epoch = sync_epoch,
    });
    startNextSnapshotTask();
    if (availability_changed) {
        reapplyRemoteItems();
    }
    emit stateChanged();
}

void ReviewRemoteLibraryCoordinator::syncNow() {
    syncAll();
}

void ReviewRemoteLibraryCoordinator::syncAll() {
    const QVector<RemoteLibraryConnection> connections = connection_store_.connections();
    for (const auto& connection : connections) {
        syncNow(connection.id);
    }
}

void ReviewRemoteLibraryCoordinator::reapplyRemoteItems() {
    (void)model_->replaceRemoteItems(projectedRemoteItems());
}

void ReviewRemoteLibraryCoordinator::materializeForEdit(const QString& presentation_photo_id) {
    if (materializing()) {
        return;
    }
    (void)startMaterialization(presentation_photo_id, MaterializationPurpose::Edit);
}

bool ReviewRemoteLibraryCoordinator::prepareExport(const QVariantList& targets) {
    if (materializing()) {
        setStatus(QStringLiteral("operation-busy"));
        return false;
    }
    if (targets.isEmpty()) {
        setStatus(QStringLiteral("remote-photo-unavailable"));
        return false;
    }
    pending_export_targets_ = targets;
    pending_export_index_ = 0;
    continueExportPreparation();
    return true;
}

bool ReviewRemoteLibraryCoordinator::startMaterialization(
    const QString& presentation_photo_id,
    const MaterializationPurpose purpose
) {
    const auto aggregate = photo_aggregates_.constFind(presentation_photo_id);
    if (materialization_attempt_photo_id_ != presentation_photo_id) {
        materialization_attempt_photo_id_ = presentation_photo_id;
        materialization_attempted_connections_.clear();
    }
    const RemotePhotoSourceChoice* source = nullptr;
    if (aggregate != photo_aggregates_.cend()) {
        const auto usable = [this](const RemotePhotoSourceChoice& candidate) {
            return !materialization_attempted_connections_.contains(candidate.connection_id)
                   && (cachedOriginalAvailable(candidate.photo)
                       || (candidate.originals_available
                           && online_connection_ids_.contains(candidate.connection_id)));
        };
        const auto* preferred = aggregate->preferredSource();
        if (preferred != nullptr && usable(*preferred))
            source = preferred;
        if (source == nullptr) {
            for (const auto& candidate : aggregate->sources) {
                if (usable(candidate)) {
                    source = &candidate;
                    break;
                }
            }
        }
    }
    const QString connection_id = source == nullptr ? QString{} : source->connection_id;
    const RemoteLibraryConnection* source_connection = connection(connection_id);
    const auto snapshot = snapshots_.constFind(connection_id);
    if (source == nullptr || snapshot == snapshots_.cend()) {
        setStatus(QStringLiteral("remote-photo-unavailable"));
        return false;
    }
    const bool cached = cachedOriginalAvailable(source->photo);
    QString server_address;
    QString authorization_value;
    if (!cached) {
        if (!online_connection_ids_.contains(connection_id)) {
            setConnectionStatus(connection_id, QStringLiteral("remote-server-offline"));
            return false;
        }
        if (!snapshot->has_server || !snapshot->server.originals_available) {
            setConnectionStatus(connection_id, QStringLiteral("remote-original-unavailable"));
            return false;
        }
        const SecretStoreResult authorization = readAuthorization(connection_id);
        if (source_connection == nullptr || !authorization.succeeded()
            || !validToken(authorization.value) || !validAddress(source_connection->address)) {
            setConnectionStatus(
                connection_id,
                QStringLiteral("connection-required"),
                authorization.diagnostic
            );
            return false;
        }
        server_address = source_connection->address;
        authorization_value = authorization.value;
    }
    materialization_attempted_connections_.insert(connection_id);
    materialization_purpose_ = purpose;
    materializing_connection_id_ = connection_id;
    materializing_photo_id_ = presentation_photo_id;
    setConnectionStatus(
        connection_id,
        cached ? QStringLiteral("preparing-cached-original")
               : QStringLiteral("downloading-original")
    );
    emit stateChanged();
    materialize_watcher_.setFuture(
        QtConcurrent::run(
            &ReviewRemoteLibraryCoordinator::runMaterializeTask,
            operations_,
            connection_id,
            presentation_photo_id,
            server_address,
            authorization_value,
            source->photo.remote_photo_id,
            source->photo.remote_representation_id
        )
    );
    return true;
}

void ReviewRemoteLibraryCoordinator::continueExportPreparation() {
    while (pending_export_index_ < pending_export_targets_.size()) {
        const QVariantMap target = pending_export_targets_.at(pending_export_index_).toMap();
        if (!target.value(QStringLiteral("isRemote")).toBool()) {
            ++pending_export_index_;
            continue;
        }
        const QString presentation_photo_id = target.value(QStringLiteral("photoId")).toString();
        if (!startMaterialization(presentation_photo_id, MaterializationPurpose::Export)) {
            failExportPreparation(status_code_);
        }
        return;
    }

    const QVariantList resolved = pending_export_targets_;
    pending_export_targets_.clear();
    pending_export_index_ = 0;
    emit exportReady(resolved);
}

void ReviewRemoteLibraryCoordinator::failExportPreparation(const QString& status_code) {
    pending_export_targets_.clear();
    pending_export_index_ = 0;
    emit exportPreparationFailed(status_code);
}

bool ReviewRemoteLibraryCoordinator::setDecision(
    const QString& presentation_photo_id,
    const BackendReviewDecisionFlag flag,
    const int rating
) {
    const auto found = photos_.find(presentation_photo_id);
    const QString connection_id = photo_connection_ids_.value(presentation_photo_id);
    if (found == photos_.end() || connection_id.isEmpty() || rating < 0 || rating > 5) {
        return false;
    }
    found->decision_flag = flag;
    found->decision_rating = static_cast<std::uint8_t>(rating);
    found->review_updated_at_ms = QDateTime::currentMSecsSinceEpoch();
    (void)model_->updateDecision(presentation_photo_id, 0, decisionFlagName(flag), rating);
    const auto aggregate = photo_aggregates_.find(presentation_photo_id);
    if (aggregate == photo_aggregates_.end()) {
        return false;
    }
    bool enqueued = false;
    for (auto& source : aggregate->sources) {
        source.photo.decision_flag = found->decision_flag;
        source.photo.decision_rating = found->decision_rating;
        source.photo.review_updated_at_ms = found->review_updated_at_ms;
        enqueued = enqueueMutation({
                       .connection_id = source.connection_id,
                       .presentation_photo_id = presentation_photo_id,
                       .remote_photo_id = source.photo.remote_photo_id,
                       .remote_representation_id = source.photo.remote_representation_id,
                       .flag = found->decision_flag,
                       .rating = found->decision_rating,
                       .liked = found->liked,
                       .color_label = found->color_label,
                       .updated_at_ms = found->review_updated_at_ms,
                   })
                   || enqueued;
    }
    emit systemCollectionCountsChanged();
    return enqueued;
}

bool ReviewRemoteLibraryCoordinator::setAffinity(
    const QString& presentation_photo_id,
    const bool liked,
    const QString& color_label
) {
    const auto found = photos_.find(presentation_photo_id);
    const QString connection_id = photo_connection_ids_.value(presentation_photo_id);
    if (found == photos_.end() || connection_id.isEmpty() || !validColorLabel(color_label)) {
        return false;
    }
    found->liked = liked;
    found->color_label = color_label;
    found->review_updated_at_ms = QDateTime::currentMSecsSinceEpoch();
    (void)model_->updateLibraryState(
        presentation_photo_id,
        liked,
        color_label,
        found->review_updated_at_ms
    );
    const auto aggregate = photo_aggregates_.find(presentation_photo_id);
    if (aggregate == photo_aggregates_.end()) {
        return false;
    }
    bool enqueued = false;
    for (auto& source : aggregate->sources) {
        source.photo.liked = found->liked;
        source.photo.color_label = found->color_label;
        source.photo.review_updated_at_ms = found->review_updated_at_ms;
        enqueued = enqueueMutation({
                       .connection_id = source.connection_id,
                       .presentation_photo_id = presentation_photo_id,
                       .remote_photo_id = source.photo.remote_photo_id,
                       .remote_representation_id = source.photo.remote_representation_id,
                       .flag = found->decision_flag,
                       .rating = found->decision_rating,
                       .liked = found->liked,
                       .color_label = found->color_label,
                       .updated_at_ms = found->review_updated_at_ms,
                   })
                   || enqueued;
    }
    emit systemCollectionCountsChanged();
    return enqueued;
}

ReviewRemoteLibraryCoordinator::SnapshotTaskResult ReviewRemoteLibraryCoordinator::runSnapshotTask(
    Operations operations,
    const SnapshotTaskKind kind,
    QString connection_id,
    QString server_address,
    QString authorization,
    const std::uint64_t job_id,
    const std::uint64_t sync_epoch
) {
    SnapshotTaskResult result;
    result.kind = kind;
    result.connection_id = connection_id;
    result.sync_epoch = sync_epoch;
    try {
        if (kind == SnapshotTaskKind::LoadCached) {
            result.snapshot = operations.snapshot(connection_id);
        } else if (kind == SnapshotTaskKind::BeginSync) {
            result.sync_start = operations.begin_sync(connection_id, server_address, authorization);
            result.snapshot = result.sync_start.snapshot;
        } else {
            result.sync_step = operations.sync_step(job_id);
            result.snapshot = result.sync_step.snapshot;
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("Unknown remote Library failure");
    }
    if (!result.error.isEmpty() && kind != SnapshotTaskKind::LoadCached) {
        try {
            result.snapshot = operations.snapshot(connection_id);
        } catch (...) {}
    }
    // File facts are refreshed even while the server is offline, outside the GUI thread.
    for (auto& photo : result.snapshot.photos) {
        photo.has_preview = photo.has_preview && QFileInfo(photo.preview_path).isFile()
                            && QFileInfo(photo.preview_path).isReadable();
        photo.has_cached_original = cachedOriginalAvailable(photo);
    }
    return result;
}

ReviewRemoteLibraryCoordinator::MaterializeTaskResult
ReviewRemoteLibraryCoordinator::runMaterializeTask(
    Operations operations,
    QString connection_id,
    QString presentation_photo_id,
    QString server_address,
    QString authorization,
    QString remote_photo_id,
    QString remote_representation_id
) {
    MaterializeTaskResult result;
    result.connection_id = std::move(connection_id);
    result.presentation_photo_id = std::move(presentation_photo_id);
    try {
        result.materialization = operations.materialize(
            result.connection_id,
            server_address,
            authorization,
            remote_photo_id,
            remote_representation_id
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("Unknown remote original materialization failure");
    }
    return result;
}

ReviewRemoteLibraryCoordinator::MutationTaskResult
ReviewRemoteLibraryCoordinator::runMutationTask(Operations operations, MutationRequest request) {
    MutationTaskResult result;
    result.presentation_photo_id = request.presentation_photo_id;
    try {
        operations.set_review_state(
            request.connection_id,
            request.remote_photo_id,
            request.remote_representation_id,
            request.flag,
            request.rating,
            request.liked,
            request.color_label,
            request.updated_at_ms
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("Unknown remote review-state failure");
    }
    return result;
}

void ReviewRemoteLibraryCoordinator::finishSnapshotTask() {
    const SnapshotTaskResult result = snapshot_watcher_.result();
    active_snapshot_connection_id_.clear();
    const bool is_sync = result.kind != SnapshotTaskKind::LoadCached;
    const bool stale =
        is_sync && latest_sync_epochs_.value(result.connection_id) != result.sync_epoch;
    const std::uint64_t returned_job_id =
        result.kind == SnapshotTaskKind::BeginSync  ? result.sync_start.job_id
        : result.kind == SnapshotTaskKind::SyncStep ? result.sync_step.job_id
                                                    : 0;
    if (stale) {
        std::uint64_t job_to_cancel = returned_job_id;
        const std::uint64_t tracked_job_id = active_sync_job_ids_.take(result.connection_id);
        if (job_to_cancel == 0) {
            job_to_cancel = tracked_job_id;
        }
        if (job_to_cancel != 0) {
            try {
                (void)operations_.cancel_sync(job_to_cancel);
            } catch (...) {}
        }
        startNextSnapshotTask();
        emit stateChanged();
        return;
    }
    if (!result.error.isEmpty()) {
        const std::uint64_t active_job_id = active_sync_job_ids_.take(result.connection_id);
        if (active_job_id != 0) {
            try {
                (void)operations_.cancel_sync(active_job_id);
            } catch (...) {}
        }
        if (is_sync) {
            online_connection_ids_.remove(result.connection_id);
            if (transportUnavailable(result.error)
                && syncFailureStatus(result.error) == QStringLiteral("sync-failed")) {
                offline_connection_ids_.insert(result.connection_id);
            } else {
                offline_connection_ids_.remove(result.connection_id);
            }
        }
        setConnectionStatus(
            result.connection_id,
            is_sync ? syncFailureStatus(result.error) : QStringLiteral("cache-load-failed"),
            result.error
        );
    } else if (result.kind == SnapshotTaskKind::LoadCached) {
        applySnapshot(result.connection_id, result.snapshot);
        setConnectionStatus(result.connection_id, QStringLiteral("offline-ready"));
    } else if (result.kind == SnapshotTaskKind::BeginSync) {
        offline_connection_ids_.remove(result.connection_id);
        online_connection_ids_.insert(result.connection_id);
        applySnapshot(result.connection_id, result.snapshot);
        active_sync_job_ids_.insert(result.connection_id, result.sync_start.job_id);
        SyncProgress progress;
        progress.job_id = result.sync_start.job_id;
        sync_progress_.insert(result.connection_id, progress);
        enqueueSyncStep(result.connection_id, result.sync_start.job_id, result.sync_epoch);
    } else if (active_sync_job_ids_.value(result.connection_id) != result.sync_step.job_id) {
        try {
            (void)operations_.cancel_sync(result.sync_step.job_id);
        } catch (...) {}
    } else {
        applySnapshot(result.connection_id, result.snapshot);
        SyncProgress progress;
        progress.job_id = result.sync_step.job_id;
        progress.page_count = result.sync_step.page_count;
        progress.photo_count = result.sync_step.photo_count;
        progress.preview_completed_count = result.sync_step.preview_completed_count;
        progress.downloaded_previews = result.sync_step.downloaded_previews;
        progress.preview_failures = result.sync_step.preview_failures;
        progress.removed = result.sync_step.removed;
        progress.manifest_complete = result.sync_step.manifest_complete;
        const SyncProgress previous = sync_progress_.value(result.connection_id);
        progress.diagnostic = previous.diagnostic;
        if (!result.sync_step.diagnostic.isEmpty()
            && !progress.diagnostic.contains(result.sync_step.diagnostic)) {
            if (!progress.diagnostic.isEmpty()) {
                progress.diagnostic.append(QLatin1Char('\n'));
            }
            progress.diagnostic.append(result.sync_step.diagnostic);
        }
        sync_progress_.insert(result.connection_id, progress);
        if (result.sync_step.complete) {
            active_sync_job_ids_.remove(result.connection_id);
            setConnectionStatus(
                result.connection_id,
                result.sync_step.preview_failures == 0
                    ? QStringLiteral("synchronized")
                    : QStringLiteral("synchronized-preview-limited"),
                progress.diagnostic
            );
        } else {
            enqueueSyncStep(result.connection_id, result.sync_step.job_id, result.sync_epoch);
        }
    }
    if (result.kind == SnapshotTaskKind::LoadCached) {
        // Startup is local-first: every mirror is projected before its network
        // request reaches the queue, then the existing progressive sync serves
        // as the live reachability check without hiding cached rows on failure.
        syncNow(result.connection_id);
    } else if (!result.error.isEmpty()) {
        if (result.snapshot.has_server)
            snapshots_.insert(result.connection_id, result.snapshot);
        rebuildPhotoAggregates();
        reapplyRemoteItems();
    }
    startNextSnapshotTask();
    emit stateChanged();
}

void ReviewRemoteLibraryCoordinator::finishMaterializeTask() {
    const MaterializeTaskResult result = materialize_watcher_.result();
    const MaterializationPurpose purpose = materialization_purpose_;
    materializing_connection_id_.clear();
    materializing_photo_id_.clear();
    if (!result.error.isEmpty()) {
        // Keep logical identity and try another verified original before reporting failure.
        if (startMaterialization(result.presentation_photo_id, purpose))
            return;
        materialization_attempt_photo_id_.clear();
        materialization_attempted_connections_.clear();
        setConnectionStatus(
            result.connection_id,
            QStringLiteral("materialize-failed"),
            result.error
        );
        emit stateChanged();
        if (purpose == MaterializationPurpose::Export) {
            failExportPreparation(QStringLiteral("materialize-failed"));
        }
        return;
    }
    materialization_attempt_photo_id_.clear();
    materialization_attempted_connections_.clear();
    const auto found = photos_.find(result.presentation_photo_id);
    if (found != photos_.end()) {
        found->has_cached_original = true;
        found->local_photo_id = result.materialization.local_photo_id;
        found->local_representation_id = result.materialization.local_representation_id;
        found->local_source_path = result.materialization.local_source_path;
        applyMaterializationMetadata(*found, result.materialization);
    }
    const auto aggregate = photo_aggregates_.find(result.presentation_photo_id);
    if (aggregate != photo_aggregates_.end()) {
        aggregate->has_cached_original = true;
        for (auto& source : aggregate->sources) {
            if (source.connection_id != result.connection_id) {
                continue;
            }
            source.photo.has_cached_original = true;
            source.photo.local_photo_id = result.materialization.local_photo_id;
            source.photo.local_representation_id = result.materialization.local_representation_id;
            source.photo.local_source_path = result.materialization.local_source_path;
            applyMaterializationMetadata(source.photo, result.materialization);
        }
    }
    reapplyRemoteItems();
    if (found != photos_.end()) {
        emit remoteInspectionChanged(
            result.presentation_photo_id,
            remoteInspection(
                result.presentation_photo_id,
                presentationRepresentationId(result.presentation_photo_id, *found),
                *found
            )
        );
    }
    setConnectionStatus(
        result.connection_id,
        result.materialization.inspection_diagnostic.isEmpty()
            ? QStringLiteral("original-ready")
            : QStringLiteral("original-ready-metadata-limited"),
        result.materialization.inspection_diagnostic
    );
    emit stateChanged();
    emit localLibraryRefreshRequested();
    if (purpose == MaterializationPurpose::Export) {
        if (pending_export_index_ >= pending_export_targets_.size()) {
            failExportPreparation(QStringLiteral("remote-photo-unavailable"));
            return;
        }
        QVariantMap target = pending_export_targets_.at(pending_export_index_).toMap();
        target.insert(QStringLiteral("photoId"), result.materialization.local_photo_id);
        target.insert(
            QStringLiteral("representationId"),
            result.materialization.local_representation_id
        );
        target.insert(QStringLiteral("sourcePath"), result.materialization.local_source_path);
        target.insert(QStringLiteral("sourceAvailable"), true);
        target.insert(QStringLiteral("isRemote"), false);
        pending_export_targets_[pending_export_index_] = target;
        ++pending_export_index_;
        continueExportPreparation();
        return;
    }
    BackendRemoteLibraryPhoto edit_photo =
        found == photos_.end() ? BackendRemoteLibraryPhoto{} : *found;
    applyMaterializationMetadata(edit_photo, result.materialization);
    QVariantMap capture_metadata = remoteInspection(
        result.materialization.local_photo_id,
        result.materialization.local_representation_id,
        edit_photo
    );
    capture_metadata.insert(
        QStringLiteral("available"),
        capture_metadata.value(QStringLiteral("hasMetadata"))
    );
    capture_metadata.insert(QStringLiteral("pending"), false);
    emit remotePhotoReady(
        result.materialization.local_photo_id,
        result.materialization.local_representation_id,
        result.materialization.local_source_path,
        result.materialization.title,
        capture_metadata
    );
}

void ReviewRemoteLibraryCoordinator::finishMutationTask() {
    const MutationTaskResult result = mutation_watcher_.result();
    mutation_task_active_ = false;
    if (!result.error.isEmpty()) {
        const QString connection_id = photo_connection_ids_.value(result.presentation_photo_id);
        setConnectionStatus(connection_id, QStringLiteral("review-save-failed"), result.error);
        emit stateChanged();
    }
    startMutationIfIdle();
}

void ReviewRemoteLibraryCoordinator::startNextSnapshotTask() {
    if (!active_snapshot_connection_id_.isEmpty() || snapshot_queue_.isEmpty()) {
        return;
    }
    SnapshotRequest request = snapshot_queue_.dequeue();
    active_snapshot_connection_id_ = request.connection_id;
    snapshot_watcher_.setFuture(
        QtConcurrent::run(
            &ReviewRemoteLibraryCoordinator::runSnapshotTask,
            operations_,
            request.kind,
            request.connection_id,
            request.server_address,
            request.authorization,
            request.job_id,
            request.sync_epoch
        )
    );
}

void ReviewRemoteLibraryCoordinator::enqueueSyncStep(
    const QString& connection_id,
    const std::uint64_t job_id,
    const std::uint64_t sync_epoch
) {
    snapshot_queue_.enqueue({
        .kind = SnapshotTaskKind::SyncStep,
        .connection_id = connection_id,
        .job_id = job_id,
        .sync_epoch = sync_epoch,
    });
}

void ReviewRemoteLibraryCoordinator::startMutationIfIdle() {
    if (mutation_task_active_ || mutation_queue_.isEmpty()) {
        return;
    }
    MutationRequest request = mutation_queue_.dequeue();
    mutation_task_active_ = true;
    mutation_watcher_.setFuture(
        QtConcurrent::run(
            &ReviewRemoteLibraryCoordinator::runMutationTask,
            operations_,
            std::move(request)
        )
    );
}

void ReviewRemoteLibraryCoordinator::applySnapshot(
    const QString& connection_id,
    BackendRemoteLibrarySnapshot snapshot
) {
    snapshots_.insert(connection_id, std::move(snapshot));
    rebuildPhotoAggregates();
    reapplyRemoteItems();
}

void ReviewRemoteLibraryCoordinator::rebuildPhotoAggregates() {
    photo_aggregates_ = aggregateRemotePhotos(snapshots_, online_connection_ids_);
    photos_.clear();
    photo_connection_ids_.clear();
    for (auto aggregate = photo_aggregates_.cbegin(); aggregate != photo_aggregates_.cend();
         ++aggregate) {
        const RemotePhotoSourceChoice* preferred = aggregate->preferredSource();
        if (preferred == nullptr) {
            continue;
        }
        BackendRemoteLibraryPhoto projected = preferred->photo;
        if (!projected.has_preview) {
            for (const auto& candidate : aggregate->sources) {
                if (!candidate.photo.has_preview)
                    continue;
                projected.has_preview = true;
                projected.preview_path = candidate.photo.preview_path;
                projected.preview_auto_transform = candidate.photo.preview_auto_transform;
                projected.preview_width = candidate.photo.preview_width;
                projected.preview_height = candidate.photo.preview_height;
                projected.preview_role = candidate.photo.preview_role;
                projected.preview_unavailable_reason.clear();
                break;
            }
        }
        for (const auto& source : aggregate->sources) {
            if (source.photo.review_updated_at_ms > projected.review_updated_at_ms) {
                projected.decision_flag = source.photo.decision_flag;
                projected.decision_rating = source.photo.decision_rating;
                projected.liked = source.photo.liked;
                projected.color_label = source.photo.color_label;
                projected.review_updated_at_ms = source.photo.review_updated_at_ms;
            }
        }
        projected.representation_count = aggregate->representation_count;
        projected.source_location_count = aggregate->source_location_count;
        projected.has_raw_representation = aggregate->has_raw_representation;
        projected.has_raster_representation = aggregate->has_raster_representation;
        photos_.insert(aggregate.key(), std::move(projected));
        photo_connection_ids_.insert(aggregate.key(), preferred->connection_id);
    }
}

QVector<ReviewItem> ReviewRemoteLibraryCoordinator::projectedRemoteItems() const {
    QVector<ReviewItem> items;
    items.reserve(photos_.size());
    for (auto found = photos_.cbegin(); found != photos_.cend(); ++found) {
        const auto& source = found.value();
        const QString connection_id = photo_connection_ids_.value(found.key());
        const auto snapshot = snapshots_.constFind(connection_id);
        const bool originals_available = snapshot != snapshots_.cend() && snapshot->has_server
                                         && snapshot->server.originals_available
                                         && online_connection_ids_.contains(connection_id);
        const bool cached_original = cachedOriginalAvailable(source);
        ReviewItem item;
        item.photo_id = found.key();
        item.representation_id = presentationRepresentationId(found.key(), source);
        item.representation_count = source.representation_count;
        item.source_location_count = source.source_location_count;
        item.has_raw_representation = source.has_raw_representation;
        item.has_raster_representation = source.has_raster_representation;
        item.visual_source_override =
            source.has_preview ? QUrl::fromLocalFile(source.preview_path).toString() : QString{};
        item.visual_auto_transform = source.preview_auto_transform;
        item.is_remote = true;
        const auto aggregate = photo_aggregates_.constFind(found.key());
        item.remote_offline = aggregate != photo_aggregates_.cend() && !aggregate->sources.isEmpty()
                              && std::all_of(
                                  aggregate->sources.cbegin(),
                                  aggregate->sources.cend(),
                                  [this](const auto& origin) {
                                      return offline_connection_ids_.contains(origin.connection_id);
                                  }
                              );
        item.remote_original_cached = cached_original;
        item.remote_connection_id = connection_id;
        item.remote_server_id = source.server_id;
        item.remote_photo_id = source.remote_photo_id;
        item.remote_representation_id = source.remote_representation_id;
        item.remote_preview_unavailable_reason = source.preview_unavailable_reason;
        item.local_backing_photo_id = source.local_photo_id;
        item.local_backing_representation_id = source.local_representation_id;
        item.decision_flag = decisionFlagName(source.decision_flag);
        item.decision_rating = source.decision_rating;
        item.liked = source.liked;
        item.color_label = source.color_label;
        item.library_state_updated_at_ms = source.review_updated_at_ms;
        item.title = source.title;
        item.source_path = cached_original ? source.local_source_path : QString{};
        item.source_available = cached_original || originals_available;
        item.visual_role = source.preview_role;
        item.visual_width = source.preview_width;
        item.visual_height = source.preview_height;
        item.has_visual = source.has_preview;
        item.capture_day =
            source.has_captured_at
                ? QDateTime::fromSecsSinceEpoch(source.captured_at_unix_seconds, QTimeZone::UTC)
                      .date()
                      .toString(Qt::ISODate)
                : QString{};
        item.metadata_schema_version = source.metadata_schema_version;
        item.has_metadata = source.has_captured_at || !source.camera_make.isEmpty()
                            || !source.camera_model.isEmpty() || !source.lens_make.isEmpty()
                            || !source.lens_model.isEmpty() || source.has_iso_speed
                            || source.has_exposure_time || source.has_aperture
                            || source.has_focal_length || source.has_focal_length_35mm
                            || source.has_raw_dimensions || source.has_image_dimensions
                            || source.has_orientation || source.has_coordinates;
        item.camera_make = source.camera_make;
        item.camera_model = source.camera_model;
        item.lens_make = source.lens_make;
        item.lens_model = source.lens_model;
        item.captured_at_unix_seconds =
            source.has_captured_at ? source.captured_at_unix_seconds : 0;
        item.iso_speed = source.has_iso_speed ? source.iso_speed : 0.0;
        item.exposure_time_seconds = source.has_exposure_time ? source.exposure_time_seconds : 0.0;
        item.aperture_f_number = source.has_aperture ? source.aperture_f_number : 0.0;
        item.focal_length_mm = source.has_focal_length ? source.focal_length_mm : 0.0;
        item.focal_length_35mm = source.has_focal_length_35mm ? source.focal_length_35mm : 0.0;
        item.raw_width = source.has_raw_dimensions ? source.raw_width : 0;
        item.raw_height = source.has_raw_dimensions ? source.raw_height : 0;
        item.image_width = source.has_image_dimensions ? source.image_width : 0;
        item.image_height = source.has_image_dimensions ? source.image_height : 0;
        item.has_orientation = source.has_orientation;
        item.orientation = source.has_orientation ? source.orientation : 0;
        item.has_coordinates = source.has_coordinates;
        item.latitude_degrees = source.has_coordinates ? source.latitude_degrees : 0.0;
        item.longitude_degrees = source.has_coordinates ? source.longitude_degrees : 0.0;
        item.has_altitude = source.has_altitude;
        item.altitude_meters = source.has_altitude ? source.altitude_meters : 0.0;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const ReviewItem& left, const ReviewItem& right) {
        if (left.captured_at_unix_seconds != right.captured_at_unix_seconds) {
            return left.captured_at_unix_seconds > right.captured_at_unix_seconds;
        }
        return left.photo_id < right.photo_id;
    });
    return items;
}

SecretStoreResult
ReviewRemoteLibraryCoordinator::readAuthorization(const QString& connection_id) const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("Shadow's local credential file is unavailable."),
        };
    }
    const RemoteLibraryConnection* requested = connection(connection_id);
    if (requested == nullptr) {
        return {
            .status = SecretStoreStatus::NotFound,
            .diagnostic = QStringLiteral("The remote Library connection no longer exists."),
        };
    }
    return secret_store_->read(QString::fromLatin1(secret_service), tokenAccount(*requested));
}

const RemoteLibraryConnection*
ReviewRemoteLibraryCoordinator::connection(const QString& connection_id) const {
    for (const auto& connection : connection_store_.connections()) {
        if (connection.id == connection_id) {
            return &connection;
        }
    }
    return nullptr;
}

QString
ReviewRemoteLibraryCoordinator::tokenAccount(const RemoteLibraryConnection& connection) const {
    return connection.uses_legacy_secret
               ? QString::fromLatin1(legacy_secret_account)
               : QString::fromLatin1(secret_account_prefix) + connection.id;
}

void ReviewRemoteLibraryCoordinator::migrateLegacySecret() {
    const QVector<RemoteLibraryConnection> connections = connection_store_.connections();
    for (const auto& connection : connections) {
        if (!connection.uses_legacy_secret) {
            continue;
        }
        if (!connection.token_stored) {
            (void)connection_store_.finishLegacySecretMigration(connection.id);
            continue;
        }
        if (!secureStorageAvailable()) {
            return;
        }
        const SecretStoreResult legacy = secret_store_->read(
            QString::fromLatin1(secret_service),
            QString::fromLatin1(legacy_secret_account)
        );
        if (legacy.status == SecretStoreStatus::NotFound) {
            (void)connection_store_.setTokenStored(connection.id, false);
            (void)connection_store_.finishLegacySecretMigration(connection.id);
            continue;
        }
        if (!legacy.succeeded()) {
            setConnectionStatus(
                connection.id,
                QStringLiteral("secret-store-failed"),
                legacy.diagnostic
            );
            continue;
        }
        const SecretStoreResult stored = secret_store_->write(
            QString::fromLatin1(secret_service),
            QString::fromLatin1(secret_account_prefix) + connection.id,
            legacy.value
        );
        if (!stored.succeeded()) {
            setConnectionStatus(
                connection.id,
                QStringLiteral("secret-store-failed"),
                stored.diagnostic
            );
            continue;
        }
        (void)secret_store_->remove(
            QString::fromLatin1(secret_service),
            QString::fromLatin1(legacy_secret_account)
        );
        (void)connection_store_.finishLegacySecretMigration(connection.id);
    }
}

void ReviewRemoteLibraryCoordinator::setStatus(const QString& code, const QString& diagnostic) {
    if (status_code_ == code && diagnostic_text_ == diagnostic) {
        return;
    }
    status_code_ = code;
    diagnostic_text_ = diagnostic;
    emit stateChanged();
}

void ReviewRemoteLibraryCoordinator::setConnectionStatus(
    const QString& connection_id,
    const QString& code,
    const QString& diagnostic
) {
    if (!connection_id.isEmpty()) {
        connection_status_codes_.insert(connection_id, code);
        connection_diagnostics_.insert(connection_id, diagnostic);
    }
    setStatus(code, diagnostic);
}

bool ReviewRemoteLibraryCoordinator::enqueueMutation(MutationRequest request) {
    mutation_queue_.enqueue(std::move(request));
    startMutationIfIdle();
    return true;
}

QString ReviewRemoteLibraryCoordinator::presentationRepresentationId(
    const QString& presentation_photo_id,
    const BackendRemoteLibraryPhoto& photo
) {
    constexpr auto content_prefix = "remote-content:";
    if (presentation_photo_id.startsWith(QLatin1StringView(content_prefix))) {
        return QStringLiteral("remote-representation-content:%1")
            .arg(presentation_photo_id.sliced(QLatin1StringView(content_prefix).size()));
    }
    return QStringLiteral("remote:%1:%2").arg(photo.server_id, photo.remote_representation_id);
}

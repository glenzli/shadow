#include "review_remote_library_coordinator.hpp"

#include <QDateTime>
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
    migrateLegacySecret();
}

ReviewRemoteLibraryCoordinator::~ReviewRemoteLibraryCoordinator() {
    snapshot_watcher_.waitForFinished();
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

QVariantList ReviewRemoteLibraryCoordinator::connections() const {
    QVariantList result;
    result.reserve(connection_store_.connections().size());
    for (const auto& connection : connection_store_.connections()) {
        const auto snapshot = snapshots_.constFind(connection.id);
        const bool has_server = snapshot != snapshots_.cend() && snapshot->has_server;
        result.push_back(
            QVariantMap{
                {QStringLiteral("id"), connection.id},
                {QStringLiteral("address"), connection.address},
                {QStringLiteral("tokenStored"), connection.token_stored},
                {QStringLiteral("connected"),
                 connection.token_stored && !connection.address.isEmpty()},
                {QStringLiteral("serverName"),
                 has_server ? snapshot->server.display_name : QString{}},
                {QStringLiteral("hasCachedServer"), has_server},
                {QStringLiteral("originalsAvailable"),
                 has_server && snapshot->server.originals_available},
                {QStringLiteral("photoCount"),
                 snapshot == snapshots_.cend() ? 0 : snapshot->photos.size()},
                {QStringLiteral("statusCode"), connection_status_codes_.value(connection.id)},
                {QStringLiteral("diagnosticText"), connection_diagnostics_.value(connection.id)},
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
    const bool already_queued = std::any_of(
        snapshot_queue_.cbegin(),
        snapshot_queue_.cend(),
        [&connection_id](const SnapshotRequest& request) {
            return request.kind == SnapshotTaskKind::Sync && request.connection_id == connection_id;
        }
    );
    if (already_queued || active_snapshot_connection_id_ == connection_id) {
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
    setConnectionStatus(connection_id, QStringLiteral("synchronizing"));
    snapshot_queue_.enqueue({
        .kind = SnapshotTaskKind::Sync,
        .connection_id = connection_id,
        .server_address = requested->address,
        .authorization = authorization.value,
    });
    startNextSnapshotTask();
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
    if (busy()) {
        return;
    }
    const auto aggregate = photo_aggregates_.constFind(presentation_photo_id);
    const RemotePhotoSourceChoice* source =
        aggregate == photo_aggregates_.cend() ? nullptr : aggregate->preferredSource();
    const QString connection_id = source == nullptr ? QString{} : source->connection_id;
    const RemoteLibraryConnection* source_connection = connection(connection_id);
    const auto snapshot = snapshots_.constFind(connection_id);
    if (source == nullptr || source_connection == nullptr || snapshot == snapshots_.cend()) {
        setStatus(QStringLiteral("remote-photo-unavailable"));
        return;
    }
    if (!snapshot->has_server || !snapshot->server.originals_available) {
        setConnectionStatus(connection_id, QStringLiteral("remote-original-unavailable"));
        return;
    }
    const SecretStoreResult authorization = readAuthorization(connection_id);
    if (!authorization.succeeded() || !validToken(authorization.value)
        || !validAddress(source_connection->address)) {
        setConnectionStatus(
            connection_id,
            QStringLiteral("connection-required"),
            authorization.diagnostic
        );
        return;
    }
    materializing_connection_id_ = connection_id;
    materializing_photo_id_ = presentation_photo_id;
    setConnectionStatus(connection_id, QStringLiteral("downloading-original"));
    emit stateChanged();
    materialize_watcher_.setFuture(
        QtConcurrent::run(
            &ReviewRemoteLibraryCoordinator::runMaterializeTask,
            operations_,
            connection_id,
            presentation_photo_id,
            source_connection->address,
            authorization.value,
            source->photo.remote_photo_id,
            source->photo.remote_representation_id
        )
    );
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
    return enqueued;
}

ReviewRemoteLibraryCoordinator::SnapshotTaskResult ReviewRemoteLibraryCoordinator::runSnapshotTask(
    Operations operations,
    const SnapshotTaskKind kind,
    QString connection_id,
    QString server_address,
    QString authorization
) {
    SnapshotTaskResult result;
    result.kind = kind;
    result.connection_id = connection_id;
    try {
        if (kind == SnapshotTaskKind::LoadCached) {
            result.snapshot = operations.snapshot(connection_id);
        } else {
            result.sync_result = operations.sync(connection_id, server_address, authorization);
            result.snapshot = result.sync_result.snapshot;
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("Unknown remote Library failure");
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
    if (!result.error.isEmpty()) {
        setConnectionStatus(
            result.connection_id,
            result.kind == SnapshotTaskKind::Sync ? QStringLiteral("sync-failed")
                                                  : QStringLiteral("cache-load-failed"),
            result.error
        );
    } else {
        applySnapshot(result.connection_id, result.snapshot);
        setConnectionStatus(
            result.connection_id,
            result.kind == SnapshotTaskKind::Sync ? QStringLiteral("synchronized")
                                                  : QStringLiteral("offline-ready")
        );
    }
    startNextSnapshotTask();
    emit stateChanged();
}

void ReviewRemoteLibraryCoordinator::finishMaterializeTask() {
    const MaterializeTaskResult result = materialize_watcher_.result();
    materializing_connection_id_.clear();
    materializing_photo_id_.clear();
    if (!result.error.isEmpty()) {
        setConnectionStatus(
            result.connection_id,
            QStringLiteral("materialize-failed"),
            result.error
        );
        emit stateChanged();
        return;
    }
    const auto found = photos_.find(result.presentation_photo_id);
    if (found != photos_.end()) {
        found->is_materialized = true;
    }
    reapplyRemoteItems();
    setConnectionStatus(result.connection_id, QStringLiteral("original-ready"));
    emit stateChanged();
    emit localLibraryRefreshRequested();
    emit remotePhotoReady(
        result.materialization.local_photo_id,
        result.materialization.local_representation_id,
        result.materialization.local_source_path,
        result.materialization.title
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
            request.authorization
        )
    );
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
    photo_aggregates_ = aggregateRemotePhotos(snapshots_);
    photos_.clear();
    photo_connection_ids_.clear();
    for (auto aggregate = photo_aggregates_.cbegin(); aggregate != photo_aggregates_.cend();
         ++aggregate) {
        if (aggregate->is_materialized) {
            continue;
        }
        const RemotePhotoSourceChoice* preferred = aggregate->preferredSource();
        if (preferred == nullptr) {
            continue;
        }
        BackendRemoteLibraryPhoto projected = preferred->photo;
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
        if (source.is_materialized) {
            continue;
        }
        const QString connection_id = photo_connection_ids_.value(found.key());
        const auto snapshot = snapshots_.constFind(connection_id);
        const bool originals_available = snapshot != snapshots_.cend() && snapshot->has_server
                                         && snapshot->server.originals_available;
        ReviewItem item;
        item.photo_id = found.key();
        item.representation_id = presentationRepresentationId(found.key(), source);
        item.representation_count = source.representation_count;
        item.source_location_count = source.source_location_count;
        item.has_raw_representation = source.has_raw_representation;
        item.has_raster_representation = source.has_raster_representation;
        item.visual_source_override =
            source.has_preview ? QUrl::fromLocalFile(source.preview_path).toString() : QString{};
        item.is_remote = true;
        item.remote_server_id = source.server_id;
        item.remote_photo_id = source.remote_photo_id;
        item.remote_representation_id = source.remote_representation_id;
        item.remote_preview_unavailable_reason = source.preview_unavailable_reason;
        item.decision_flag = decisionFlagName(source.decision_flag);
        item.decision_rating = source.decision_rating;
        item.liked = source.liked;
        item.color_label = source.color_label;
        item.library_state_updated_at_ms = source.review_updated_at_ms;
        item.title = source.title;
        item.source_available = originals_available;
        item.visual_role = source.preview_role;
        item.visual_width = source.preview_width;
        item.visual_height = source.preview_height;
        item.has_visual = source.has_preview;
        item.has_metadata = source.has_captured_at || !source.camera_make.isEmpty()
                            || !source.camera_model.isEmpty() || !source.lens_make.isEmpty()
                            || !source.lens_model.isEmpty() || source.has_iso_speed
                            || source.has_exposure_time || source.has_aperture
                            || source.has_focal_length || source.has_raw_dimensions;
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
        item.raw_width = source.has_raw_dimensions ? source.raw_width : 0;
        item.raw_height = source.has_raw_dimensions ? source.raw_height : 0;
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
            .diagnostic = QStringLiteral("The operating system credential store is unavailable."),
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

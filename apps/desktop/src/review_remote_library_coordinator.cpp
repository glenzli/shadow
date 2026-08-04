#include "review_remote_library_coordinator.hpp"

#include <QDateTime>
#include <QSettings>
#include <QUrl>
#include <QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace {

constexpr auto server_address_key = "remote_library/server_address";
constexpr auto token_stored_key = "remote_library/token_stored";
constexpr auto secret_service = "dev.shadow.photo.remote-library";
constexpr auto secret_account = "library-sharing-token";

[[nodiscard]] std::unique_ptr<QSettings> makeSettings(const QString& isolated_settings_file) {
    if (isolated_settings_file.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(isolated_settings_file, QSettings::IniFormat);
}

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
    settings_(makeSettings(isolated_settings_file)), secret_store_(std::move(secret_store)),
    server_address_(settings_->value(QString::fromLatin1(server_address_key)).toString().trimmed()),
    token_stored_(settings_->value(QString::fromLatin1(token_stored_key), false).toBool()) {
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
    if (started_ || snapshot_watcher_.isRunning()) {
        return;
    }
    started_ = true;
    emit stateChanged();
    snapshot_watcher_.setFuture(QtConcurrent::run(
        &ReviewRemoteLibraryCoordinator::runSnapshotTask,
        operations_,
        SnapshotTaskKind::LoadCached,
        QString{},
        QString{}
    ));
}

bool ReviewRemoteLibraryCoordinator::busy() const noexcept {
    return snapshot_watcher_.isRunning() || materialize_watcher_.isRunning();
}

bool ReviewRemoteLibraryCoordinator::syncing() const noexcept {
    return snapshot_watcher_.isRunning();
}

bool ReviewRemoteLibraryCoordinator::materializing() const noexcept {
    return materialize_watcher_.isRunning();
}

bool ReviewRemoteLibraryCoordinator::secureStorageAvailable() const noexcept {
    return secret_store_ != nullptr && secret_store_->available();
}

bool ReviewRemoteLibraryCoordinator::tokenStored() const noexcept {
    return token_stored_;
}

QString ReviewRemoteLibraryCoordinator::serverAddress() const {
    return server_address_;
}

bool ReviewRemoteLibraryCoordinator::hasServer() const noexcept {
    return snapshot_.has_server;
}

QString ReviewRemoteLibraryCoordinator::serverName() const {
    return snapshot_.has_server ? snapshot_.server.display_name : QString{};
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

bool ReviewRemoteLibraryCoordinator::saveConnection(
    const QString& server_address,
    const QString& token
) {
    const QString normalized_address = server_address.trimmed();
    const QString normalized_token = token.trimmed();
    if (!validAddress(normalized_address)) {
        setStatus(QStringLiteral("invalid-address"));
        return false;
    }
    if (!validToken(normalized_token)) {
        setStatus(QStringLiteral("invalid-token"));
        return false;
    }
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult stored = secret_store_->write(
        QString::fromLatin1(secret_service),
        QString::fromLatin1(secret_account),
        normalized_token
    );
    if (!stored.succeeded()) {
        setStatus(QStringLiteral("secret-store-failed"), stored.diagnostic);
        return false;
    }
    const bool connection_changed = server_address_ != normalized_address || !token_stored_;
    server_address_ = normalized_address;
    token_stored_ = true;
    settings_->setValue(QString::fromLatin1(server_address_key), server_address_);
    settings_->setValue(QString::fromLatin1(token_stored_key), true);
    settings_->sync();
    if (connection_changed) {
        emit connectionChanged();
    }
    setStatus(QStringLiteral("connection-saved"));
    return true;
}

bool ReviewRemoteLibraryCoordinator::removeConnection() {
    if (!secureStorageAvailable()) {
        setStatus(QStringLiteral("secure-storage-unavailable"));
        return false;
    }
    const SecretStoreResult removed = secret_store_->remove(
        QString::fromLatin1(secret_service),
        QString::fromLatin1(secret_account)
    );
    if (removed.status != SecretStoreStatus::Success
        && removed.status != SecretStoreStatus::NotFound) {
        setStatus(QStringLiteral("secret-store-failed"), removed.diagnostic);
        return false;
    }
    const bool connection_changed = !server_address_.isEmpty() || token_stored_;
    server_address_.clear();
    token_stored_ = false;
    settings_->remove(QString::fromLatin1(server_address_key));
    settings_->setValue(QString::fromLatin1(token_stored_key), false);
    settings_->sync();
    if (connection_changed) {
        emit connectionChanged();
    }
    setStatus(QStringLiteral("connection-removed"));
    return true;
}

void ReviewRemoteLibraryCoordinator::syncNow() {
    if (snapshot_watcher_.isRunning() || materialize_watcher_.isRunning()) {
        return;
    }
    if (!validAddress(server_address_) || !token_stored_) {
        setStatus(QStringLiteral("connection-required"));
        return;
    }
    const SecretStoreResult authorization = readAuthorization();
    if (!authorization.succeeded() || !validToken(authorization.value)) {
        if (authorization.status == SecretStoreStatus::NotFound) {
            token_stored_ = false;
            settings_->setValue(QString::fromLatin1(token_stored_key), false);
            settings_->sync();
            emit connectionChanged();
        }
        setStatus(QStringLiteral("token-required"), authorization.diagnostic);
        return;
    }
    setStatus(QStringLiteral("synchronizing"));
    emit stateChanged();
    snapshot_watcher_.setFuture(QtConcurrent::run(
        &ReviewRemoteLibraryCoordinator::runSnapshotTask,
        operations_,
        SnapshotTaskKind::Sync,
        server_address_,
        authorization.value
    ));
}

void ReviewRemoteLibraryCoordinator::reapplyRemoteItems() {
    (void)model_->replaceRemoteItems(projectedRemoteItems());
}

void ReviewRemoteLibraryCoordinator::materializeForEdit(
    const QString& presentation_photo_id
) {
    if (snapshot_watcher_.isRunning() || materialize_watcher_.isRunning()) {
        return;
    }
    const auto found = photos_.constFind(presentation_photo_id);
    if (found == photos_.cend()) {
        setStatus(QStringLiteral("remote-photo-unavailable"));
        return;
    }
    if (!snapshot_.server.originals_available) {
        setStatus(QStringLiteral("remote-original-unavailable"));
        return;
    }
    const SecretStoreResult authorization = readAuthorization();
    if (!authorization.succeeded() || !validToken(authorization.value)
        || !validAddress(server_address_)) {
        setStatus(QStringLiteral("connection-required"), authorization.diagnostic);
        return;
    }
    materializing_photo_id_ = presentation_photo_id;
    setStatus(QStringLiteral("downloading-original"));
    emit stateChanged();
    materialize_watcher_.setFuture(QtConcurrent::run(
        &ReviewRemoteLibraryCoordinator::runMaterializeTask,
        operations_,
        presentation_photo_id,
        server_address_,
        authorization.value,
        found->remote_photo_id,
        found->remote_representation_id
    ));
}

bool ReviewRemoteLibraryCoordinator::setDecision(
    const QString& presentation_photo_id,
    const BackendReviewDecisionFlag flag,
    const int rating
) {
    const auto found = photos_.find(presentation_photo_id);
    if (found == photos_.end() || rating < 0 || rating > 5) {
        return false;
    }
    found->decision_flag = flag;
    found->decision_rating = static_cast<std::uint8_t>(rating);
    found->review_updated_at_ms = QDateTime::currentMSecsSinceEpoch();
    (void)model_->updateDecision(
        presentation_photo_id,
        0,
        decisionFlagName(flag),
        rating
    );
    return enqueueMutation({
        .presentation_photo_id = presentation_photo_id,
        .remote_photo_id = found->remote_photo_id,
        .remote_representation_id = found->remote_representation_id,
        .flag = found->decision_flag,
        .rating = found->decision_rating,
        .liked = found->liked,
        .color_label = found->color_label,
        .updated_at_ms = found->review_updated_at_ms,
    });
}

bool ReviewRemoteLibraryCoordinator::setAffinity(
    const QString& presentation_photo_id,
    const bool liked,
    const QString& color_label
) {
    const auto found = photos_.find(presentation_photo_id);
    if (found == photos_.end() || !validColorLabel(color_label)) {
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
    return enqueueMutation({
        .presentation_photo_id = presentation_photo_id,
        .remote_photo_id = found->remote_photo_id,
        .remote_representation_id = found->remote_representation_id,
        .flag = found->decision_flag,
        .rating = found->decision_rating,
        .liked = found->liked,
        .color_label = found->color_label,
        .updated_at_ms = found->review_updated_at_ms,
    });
}

ReviewRemoteLibraryCoordinator::SnapshotTaskResult
ReviewRemoteLibraryCoordinator::runSnapshotTask(
    Operations operations,
    const SnapshotTaskKind kind,
    QString server_address,
    QString authorization
) {
    SnapshotTaskResult result;
    result.kind = kind;
    try {
        if (kind == SnapshotTaskKind::LoadCached) {
            result.snapshot = operations.snapshot();
        } else {
            result.sync_result = operations.sync(server_address, authorization);
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
    QString presentation_photo_id,
    QString server_address,
    QString authorization,
    QString remote_photo_id,
    QString remote_representation_id
) {
    MaterializeTaskResult result;
    result.presentation_photo_id = std::move(presentation_photo_id);
    try {
        result.materialization = operations.materialize(
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
ReviewRemoteLibraryCoordinator::runMutationTask(
    Operations operations,
    MutationRequest request
) {
    MutationTaskResult result;
    result.presentation_photo_id = request.presentation_photo_id;
    try {
        operations.set_review_state(
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
    if (!result.error.isEmpty()) {
        setStatus(
            result.kind == SnapshotTaskKind::Sync ? QStringLiteral("sync-failed")
                                                  : QStringLiteral("cache-load-failed"),
            result.error
        );
        emit stateChanged();
        return;
    }
    applySnapshot(result.snapshot);
    setStatus(
        result.kind == SnapshotTaskKind::Sync ? QStringLiteral("synchronized")
                                              : QStringLiteral("offline-ready")
    );
    emit stateChanged();
}

void ReviewRemoteLibraryCoordinator::finishMaterializeTask() {
    const MaterializeTaskResult result = materialize_watcher_.result();
    materializing_photo_id_.clear();
    if (!result.error.isEmpty()) {
        setStatus(QStringLiteral("materialize-failed"), result.error);
        emit stateChanged();
        return;
    }
    const auto found = photos_.find(result.presentation_photo_id);
    if (found != photos_.end()) {
        found->is_materialized = true;
    }
    reapplyRemoteItems();
    setStatus(QStringLiteral("original-ready"));
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
    if (!result.error.isEmpty()) {
        setStatus(QStringLiteral("review-save-failed"), result.error);
        emit stateChanged();
    }
    startMutationIfIdle();
}

void ReviewRemoteLibraryCoordinator::startMutationIfIdle() {
    if (mutation_watcher_.isRunning() || mutation_queue_.isEmpty()) {
        return;
    }
    MutationRequest request = mutation_queue_.dequeue();
    mutation_watcher_.setFuture(QtConcurrent::run(
        &ReviewRemoteLibraryCoordinator::runMutationTask,
        operations_,
        std::move(request)
    ));
}

void ReviewRemoteLibraryCoordinator::applySnapshot(BackendRemoteLibrarySnapshot snapshot) {
    snapshot_ = std::move(snapshot);
    photos_.clear();
    for (const auto& photo : snapshot_.photos) {
        if (!photo.is_materialized) {
            photos_.insert(presentationPhotoId(photo), photo);
        }
    }
    reapplyRemoteItems();
}

QVector<ReviewItem> ReviewRemoteLibraryCoordinator::projectedRemoteItems() const {
    QVector<ReviewItem> items;
    items.reserve(photos_.size());
    for (auto found = photos_.cbegin(); found != photos_.cend(); ++found) {
        const auto& source = found.value();
        if (source.is_materialized) {
            continue;
        }
        ReviewItem item;
        item.photo_id = found.key();
        item.representation_id = presentationRepresentationId(source);
        item.visual_source_override = source.has_preview
                                          ? QUrl::fromLocalFile(source.preview_path).toString()
                                          : QString{};
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
        item.source_available = snapshot_.has_server && snapshot_.server.originals_available;
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
        item.captured_at_unix_seconds = source.has_captured_at
                                            ? source.captured_at_unix_seconds
                                            : 0;
        item.iso_speed = source.has_iso_speed ? source.iso_speed : 0.0;
        item.exposure_time_seconds = source.has_exposure_time
                                         ? source.exposure_time_seconds
                                         : 0.0;
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

SecretStoreResult ReviewRemoteLibraryCoordinator::readAuthorization() const {
    if (!secureStorageAvailable()) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .diagnostic = QStringLiteral("The operating system credential store is unavailable."),
        };
    }
    return secret_store_->read(
        QString::fromLatin1(secret_service),
        QString::fromLatin1(secret_account)
    );
}

void ReviewRemoteLibraryCoordinator::setStatus(
    const QString& code,
    const QString& diagnostic
) {
    if (status_code_ == code && diagnostic_text_ == diagnostic) {
        return;
    }
    status_code_ = code;
    diagnostic_text_ = diagnostic;
    emit stateChanged();
}

bool ReviewRemoteLibraryCoordinator::enqueueMutation(MutationRequest request) {
    mutation_queue_.enqueue(std::move(request));
    startMutationIfIdle();
    return true;
}

QString ReviewRemoteLibraryCoordinator::presentationPhotoId(
    const BackendRemoteLibraryPhoto& photo
) {
    return QStringLiteral("remote:%1:%2").arg(photo.server_id, photo.remote_photo_id);
}

QString ReviewRemoteLibraryCoordinator::presentationRepresentationId(
    const BackendRemoteLibraryPhoto& photo
) {
    return QStringLiteral("remote:%1:%2")
        .arg(photo.server_id, photo.remote_representation_id);
}

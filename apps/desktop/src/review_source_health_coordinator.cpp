#include "review_source_health_coordinator.hpp"

#include "library_source_quick_probe.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t MISSING_SOURCE_LOCATION_PAGE_SIZE = 24;

[[nodiscard]] LocalizedUiMessage source_health_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewSourceHealthCoordinator::ReviewSourceHealthCoordinator(
    Operations operations,
    QObject* parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.source_health || !operations_.remove_source || !operations_.missing_locations
        || !operations_.relink || !operations_.relink_library || !operations_.recover_source
        || !operations_.reconcile_missing || !operations_.archive_photo) {
        throw std::invalid_argument("complete Review source-health operations are required");
    }
    connect(
        &source_health_watcher_,
        &QFutureWatcher<SourceHealthTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishSourceHealthTask
    );
    connect(
        &missing_locations_watcher_,
        &QFutureWatcher<MissingLocationTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishMissingLocationTask
    );
    connect(
        &remove_source_watcher_,
        &QFutureWatcher<RemoveSourceTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishRemoveSourceTask
    );
    connect(
        &relink_watcher_,
        &QFutureWatcher<RelinkTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishRelinkTask
    );
    connect(
        &archive_photo_watcher_,
        &QFutureWatcher<ArchivePhotoTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishArchivePhotoTask
    );
    connect(
        &recover_source_watcher_,
        &QFutureWatcher<RecoverSourceTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishRecoverSourceTask
    );
    connect(
        &reconcile_watcher_,
        &QFutureWatcher<ReconcileTaskResult>::finished,
        this,
        &ReviewSourceHealthCoordinator::finishReconcileTask
    );
}

ReviewSourceHealthCoordinator::~ReviewSourceHealthCoordinator() {
    source_health_watcher_.waitForFinished();
    remove_source_watcher_.waitForFinished();
    missing_locations_watcher_.waitForFinished();
    relink_watcher_.waitForFinished();
    recover_source_watcher_.waitForFinished();
    reconcile_watcher_.waitForFinished();
    archive_photo_watcher_.waitForFinished();
}

QVariantList ReviewSourceHealthCoordinator::sourceHealth() const {
    QVariantList result;
    result.reserve(source_health_.size());
    for (const auto& source : source_health_) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("sourceId"), source.source_id},
                {QStringLiteral("sourcePath"), source.source_display_path},
                {QStringLiteral("enabled"), source.source_enabled},
                {
                    QStringLiteral("hasLatestCompletedScan"),
                    source.has_latest_completed_scan,
                },
                {QStringLiteral("scanSessionId"), source.scan_session_id},
                {QStringLiteral("scanCompletedAtMs"), source.scan_completed_at_ms},
                {QStringLiteral("knownLocations"), source.known_locations},
                {QStringLiteral("seenLocations"), source.seen_locations},
                {QStringLiteral("notSeenLocations"), source.not_seen_locations},
                {
                    QStringLiteral("sourceRootAvailable"),
                    source.source_root_available,
                },
                {
                    QStringLiteral("hasQuickInventory"),
                    source.has_quick_inventory,
                },
                {
                    QStringLiteral("currentSupportedFiles"),
                    source.current_supported_files,
                },
                {
                    QStringLiteral("quickInventoryNeedsScan"),
                    source.quick_inventory_needs_scan,
                },
                {
                    QStringLiteral("suspectedMissingLocations"),
                    source.suspected_missing_locations,
                },
            }
        );
    }
    return result;
}

bool ReviewSourceHealthCoordinator::sourceHealthBusy() const noexcept {
    return source_health_running_;
}

bool ReviewSourceHealthCoordinator::removeSourceBusy() const noexcept {
    return remove_source_running_;
}

QVariantList ReviewSourceHealthCoordinator::missingLocations() const {
    QVariantList result;
    result.reserve(missing_locations_.size());
    for (const auto& location : missing_locations_) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("locationId"), location.location_id},
                {QStringLiteral("photoId"), location.photo_id},
                {QStringLiteral("title"), location.title},
                {QStringLiteral("sourcePath"), location.source_display_path},
                {QStringLiteral("hasCapturedAt"), location.has_captured_at},
                {
                    QStringLiteral("capturedAtUnixSeconds"),
                    location.captured_at_unix_seconds,
                },
                {QStringLiteral("cameraKey"), location.camera_key},
                {QStringLiteral("lastSeenAtMs"), location.last_seen_at_ms},
            }
        );
    }
    return result;
}

QString ReviewSourceHealthCoordinator::missingLocationScanId() const {
    return missing_location_scan_id_;
}

bool ReviewSourceHealthCoordinator::missingLocationsBusy() const noexcept {
    return missing_locations_running_;
}

bool ReviewSourceHealthCoordinator::missingLocationsHasMore() const noexcept {
    return missing_locations_has_more_;
}

bool ReviewSourceHealthCoordinator::relinkBusy() const noexcept {
    return relink_running_ || recover_source_running_;
}

bool ReviewSourceHealthCoordinator::reconcileBusy() const noexcept {
    return reconcile_running_;
}

QString ReviewSourceHealthCoordinator::relinkStatusText() const {
    return relink_status_message_.translated();
}

LocalizedUiMessage ReviewSourceHealthCoordinator::globalStatusMessage() const {
    return global_status_message_;
}

void ReviewSourceHealthCoordinator::refreshSourceHealth() {
    if (source_health_running_) {
        source_health_refresh_pending_ = true;
        return;
    }
    startSourceHealthTask();
}

void ReviewSourceHealthCoordinator::removeSource(
    const QString& source_id,
    const QString& source_path
) {
    const QString normalized_source_id = source_id.trimmed();
    if (remove_source_running_ || relinkBusy() || reconcile_running_ || archive_photo_running_
        || normalized_source_id.isEmpty()) {
        return;
    }
    startRemoveSourceTask(normalized_source_id, source_path);
}

void ReviewSourceHealthCoordinator::openMissingLocationReview(const QString& scan_session_id) {
    const QString normalized_scan_id = scan_session_id.trimmed();
    if (normalized_scan_id.isEmpty()) {
        return;
    }
    missing_location_scan_id_ = normalized_scan_id;
    missing_location_next_cursor_.clear();
    missing_locations_.clear();
    missing_locations_has_more_ = false;
    if (missing_locations_running_) {
        active_missing_locations_request_id_ = ++missing_locations_request_id_;
        missing_locations_refresh_pending_ = true;
        emit missingLocationReviewChanged();
        return;
    }
    startMissingLocationTask(false);
}

void ReviewSourceHealthCoordinator::closeMissingLocationReview() {
    active_missing_locations_request_id_ = ++missing_locations_request_id_;
    missing_locations_refresh_pending_ = false;
    missing_location_scan_id_.clear();
    missing_location_next_cursor_.clear();
    missing_locations_.clear();
    missing_locations_has_more_ = false;
    emit missingLocationReviewChanged();
}

void ReviewSourceHealthCoordinator::loadMoreMissingLocations() {
    if (missing_locations_running_ || !missing_locations_has_more_
        || missing_location_scan_id_.isEmpty()) {
        return;
    }
    startMissingLocationTask(true);
}

void ReviewSourceHealthCoordinator::relinkMissingLocation(
    const QString& location_id,
    const QUrl& candidate_url
) {
    const QString normalized_location_id = location_id.trimmed();
    const QString candidate_path = candidate_url.toLocalFile();
    if (relinkBusy() || reconcile_running_ || archive_photo_running_
        || missing_location_scan_id_.isEmpty() || normalized_location_id.isEmpty()
        || candidate_path.isEmpty()) {
        return;
    }
    startRelinkTask(normalized_location_id, candidate_path);
}

void ReviewSourceHealthCoordinator::relinkUnavailableLocation(
    const QString& location_id,
    const QUrl& candidate_url
) {
    const QString normalized_location_id = location_id.trimmed();
    const QString candidate_path = candidate_url.toLocalFile();
    if (relinkBusy() || reconcile_running_ || archive_photo_running_
        || normalized_location_id.isEmpty() || candidate_path.isEmpty()) {
        return;
    }
    startLibraryRelinkTask(normalized_location_id, candidate_path);
}

void ReviewSourceHealthCoordinator::recoverSource(
    const QString& source_id,
    const QUrl& candidate_url
) {
    const QString normalized_source_id = source_id.trimmed();
    const QString candidate_path = candidate_url.toLocalFile();
    if (relinkBusy() || reconcile_running_ || archive_photo_running_
        || normalized_source_id.isEmpty() || candidate_path.isEmpty()) {
        return;
    }
    startRecoverSourceTask(normalized_source_id, candidate_path);
}

void ReviewSourceHealthCoordinator::reconcileMissing(
    const QString& scan_session_id,
    const QString& source_path
) {
    const QString normalized_scan_id = scan_session_id.trimmed();
    if (reconcile_running_ || relinkBusy() || archive_photo_running_
        || normalized_scan_id.isEmpty()) {
        return;
    }
    startReconcileTask(normalized_scan_id, source_path);
}

void ReviewSourceHealthCoordinator::archiveUnavailablePhoto(
    const QString& photo_id,
    const QString& title
) {
    const QString normalized_photo_id = photo_id.trimmed();
    if (archive_photo_running_ || relinkBusy() || reconcile_running_
        || normalized_photo_id.isEmpty()) {
        return;
    }
    startArchivePhotoTask(normalized_photo_id, title);
}

void ReviewSourceHealthCoordinator::retranslateUi() {
    if (!relink_status_message_.isEmpty()) {
        emit missingLocationReviewChanged();
    }
}

ReviewSourceHealthCoordinator::SourceHealthTaskResult
ReviewSourceHealthCoordinator::runSourceHealthTask(
    std::function<QVector<BackendLibrarySourceHealth>()> operation,
    const quint64 request_id
) {
    SourceHealthTaskResult result;
    result.request_id = request_id;
    try {
        result.sources = operation();
        for (auto& source : result.sources) {
            if (!source.source_enabled) {
                continue;
            }
            const LibrarySourceQuickProbe probe = probeLibrarySource(source.source_display_path);
            source.source_root_available = probe.root_available;
            // A missing root is itself a complete cheap observation. An
            // available root is useful only when its directory walk settled.
            source.has_quick_inventory = !probe.root_available || probe.inventory_complete;
            source.current_supported_files = probe.supported_files;
            if (!source.has_latest_completed_scan || !source.has_quick_inventory) {
                continue;
            }
            source.quick_inventory_needs_scan =
                !source.source_root_available
                || source.current_supported_files != source.known_locations;
            if (source.known_locations > source.current_supported_files) {
                source.suspected_missing_locations =
                    source.known_locations - source.current_supported_files;
            }
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::MissingLocationTaskResult
ReviewSourceHealthCoordinator::runMissingLocationTask(
    std::function<BackendMissingSourceLocationPage(
        const QString& scan_session_id,
        const QString& after_location_id,
        std::uint32_t limit
    )> operation,
    QString scan_session_id,
    QString after_location_id,
    const quint64 request_id,
    const bool append
) {
    MissingLocationTaskResult result;
    result.scan_session_id = std::move(scan_session_id);
    result.request_id = request_id;
    result.append = append;
    try {
        result.page =
            operation(result.scan_session_id, after_location_id, MISSING_SOURCE_LOCATION_PAGE_SIZE);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::RemoveSourceTaskResult
ReviewSourceHealthCoordinator::runRemoveSourceTask(
    std::function<bool(const QString& source_id)> operation,
    QString source_id,
    QString source_path,
    const quint64 request_id
) {
    RemoveSourceTaskResult result;
    result.source_id = std::move(source_id);
    result.source_path = std::move(source_path);
    result.request_id = request_id;
    try {
        result.removed = operation(result.source_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::RelinkTaskResult ReviewSourceHealthCoordinator::runRelinkTask(
    std::function<BackendVerifiedSourceRelinkReceipt(
        const QString& scan_session_id,
        const QString& location_id,
        const QString& candidate_path
    )> operation,
    QString scan_session_id,
    QString location_id,
    QString candidate_path,
    const quint64 request_id
) {
    RelinkTaskResult result;
    result.location_id = location_id;
    result.request_id = request_id;
    try {
        result.receipt = operation(scan_session_id, location_id, candidate_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::RelinkTaskResult ReviewSourceHealthCoordinator::runLibraryRelinkTask(
    std::function<BackendVerifiedSourceRelinkReceipt(
        const QString& location_id,
        const QString& candidate_path
    )> operation,
    QString location_id,
    QString candidate_path,
    const quint64 request_id
) {
    RelinkTaskResult result;
    result.location_id = location_id;
    result.request_id = request_id;
    try {
        result.receipt = operation(location_id, candidate_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::ArchivePhotoTaskResult
ReviewSourceHealthCoordinator::runArchivePhotoTask(
    std::function<bool(const QString& photo_id)> operation,
    QString photo_id,
    QString title,
    const quint64 request_id
) {
    ArchivePhotoTaskResult result;
    result.photo_id = std::move(photo_id);
    result.title = std::move(title);
    result.request_id = request_id;
    try {
        result.archived = operation(result.photo_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::RecoverSourceTaskResult
ReviewSourceHealthCoordinator::runRecoverSourceTask(
    std::function<BackendLibrarySourceRecoveryReceipt(
        const QString& source_id,
        const QString& replacement_folder
    )> operation,
    QString source_id,
    QString replacement_folder,
    const quint64 request_id
) {
    RecoverSourceTaskResult result;
    result.source_id = source_id;
    result.request_id = request_id;
    try {
        result.receipt = operation(source_id, replacement_folder);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

ReviewSourceHealthCoordinator::ReconcileTaskResult ReviewSourceHealthCoordinator::runReconcileTask(
    std::function<BackendSourceReconciliationReceipt(const QString& scan_session_id)> operation,
    QString scan_session_id,
    QString source_path,
    const quint64 request_id
) {
    ReconcileTaskResult result;
    result.scan_session_id = scan_session_id;
    result.source_path = source_path;
    result.request_id = request_id;
    try {
        result.receipt = operation(scan_session_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewSourceHealthCoordinator::startSourceHealthTask() {
    if (source_health_running_) {
        source_health_refresh_pending_ = true;
        return;
    }
    source_health_running_ = true;
    active_source_health_request_id_ = ++source_health_request_id_;
    emit sourceHealthChanged();
    source_health_watcher_.setFuture(
        QtConcurrent::run(
            runSourceHealthTask,
            operations_.source_health,
            active_source_health_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::startRemoveSourceTask(
    const QString& source_id,
    const QString& source_path
) {
    remove_source_running_ = true;
    active_remove_source_request_id_ = ++remove_source_request_id_;
    emit sourceHealthChanged();
    remove_source_watcher_.setFuture(
        QtConcurrent::run(
            runRemoveSourceTask,
            operations_.remove_source,
            source_id,
            source_path,
            active_remove_source_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::startMissingLocationTask(const bool append) {
    if (missing_locations_running_) {
        missing_locations_refresh_pending_ = true;
        return;
    }
    if (missing_location_scan_id_.isEmpty()) {
        return;
    }
    missing_locations_running_ = true;
    active_missing_locations_request_id_ = ++missing_locations_request_id_;
    emit missingLocationReviewChanged();
    missing_locations_watcher_.setFuture(
        QtConcurrent::run(
            runMissingLocationTask,
            operations_.missing_locations,
            missing_location_scan_id_,
            append ? missing_location_next_cursor_ : QString{},
            active_missing_locations_request_id_,
            append
        )
    );
}

void ReviewSourceHealthCoordinator::startRelinkTask(
    const QString& location_id,
    const QString& candidate_path
) {
    relink_running_ = true;
    relink_status_message_ =
        source_health_message(QT_TRANSLATE_NOOP("ReviewController", "Verifying selected source…"));
    active_relink_request_id_ = ++relink_request_id_;
    emit missingLocationReviewChanged();
    relink_watcher_.setFuture(
        QtConcurrent::run(
            runRelinkTask,
            operations_.relink,
            missing_location_scan_id_,
            location_id,
            candidate_path,
            active_relink_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::startLibraryRelinkTask(
    const QString& location_id,
    const QString& candidate_path
) {
    relink_running_ = true;
    relink_status_message_ =
        source_health_message(QT_TRANSLATE_NOOP("ReviewController", "Verifying selected source…"));
    active_relink_request_id_ = ++relink_request_id_;
    emit missingLocationReviewChanged();
    relink_watcher_.setFuture(
        QtConcurrent::run(
            runLibraryRelinkTask,
            operations_.relink_library,
            location_id,
            candidate_path,
            active_relink_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::startRecoverSourceTask(
    const QString& source_id,
    const QString& candidate_path
) {
    recover_source_running_ = true;
    relink_status_message_ =
        source_health_message(QT_TRANSLATE_NOOP("ReviewController", "Locating missing originals…"));
    active_recover_source_request_id_ = ++recover_source_request_id_;
    emit missingLocationReviewChanged();
    recover_source_watcher_.setFuture(
        QtConcurrent::run(
            runRecoverSourceTask,
            operations_.recover_source,
            source_id,
            candidate_path,
            active_recover_source_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::startReconcileTask(
    const QString& scan_session_id,
    const QString& source_path
) {
    reconcile_running_ = true;
    active_reconcile_request_id_ = ++reconcile_request_id_;
    emit sourceHealthChanged();
    reconcile_watcher_.setFuture(
        QtConcurrent::run(
            runReconcileTask,
            operations_.reconcile_missing,
            scan_session_id,
            source_path,
            active_reconcile_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::startArchivePhotoTask(
    const QString& photo_id,
    const QString& title
) {
    archive_photo_running_ = true;
    active_archive_photo_request_id_ = ++archive_photo_request_id_;
    archive_photo_watcher_.setFuture(
        QtConcurrent::run(
            runArchivePhotoTask,
            operations_.archive_photo,
            photo_id,
            title,
            active_archive_photo_request_id_
        )
    );
}

void ReviewSourceHealthCoordinator::finishSourceHealthTask() {
    SourceHealthTaskResult result = source_health_watcher_.result();
    source_health_running_ = false;
    const bool accepted = result.request_id == active_source_health_request_id_;
    if (accepted && result.error.isEmpty()) {
        source_health_ = std::move(result.sources);
        emit sourceHealthChanged();
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not load Library source health · %1"),
            {result.error}
        ));
        emit sourceHealthChanged();
    }

    if (source_health_refresh_pending_ || !accepted) {
        source_health_refresh_pending_ = false;
        startSourceHealthTask();
    }
}

void ReviewSourceHealthCoordinator::finishRemoveSourceTask() {
    RemoveSourceTaskResult result = remove_source_watcher_.result();
    remove_source_running_ = false;
    const bool accepted = result.request_id == active_remove_source_request_id_;
    if (accepted && result.error.isEmpty()) {
        closeMissingLocationReview();
        if (result.removed) {
            emit libraryVisibilityChanged();
        }
        publishGlobalStatus(source_health_message(
            result.removed
                ? QT_TRANSLATE_NOOP("ReviewController", "Removed Library folder · %1")
                : QT_TRANSLATE_NOOP("ReviewController", "Library folder was already removed · %1"),
            {result.source_path}
        ));
        refreshSourceHealth();
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not remove Library folder · %1"),
            {result.error}
        ));
    }
    emit sourceHealthChanged();
}

void ReviewSourceHealthCoordinator::finishMissingLocationTask() {
    MissingLocationTaskResult result = missing_locations_watcher_.result();
    missing_locations_running_ = false;
    const bool accepted = result.request_id == active_missing_locations_request_id_
                          && result.scan_session_id == missing_location_scan_id_;
    if (accepted && result.error.isEmpty()) {
        if (result.page.has_scan) {
            if (result.append) {
                missing_locations_ += std::move(result.page.items);
            } else {
                missing_locations_ = std::move(result.page.items);
            }
            missing_location_next_cursor_ = std::move(result.page.next_location_id);
            missing_locations_has_more_ = result.page.has_more;
        } else {
            missing_locations_.clear();
            missing_location_next_cursor_.clear();
            missing_locations_has_more_ = false;
        }
        emit missingLocationReviewChanged();
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not load source scan review · %1"),
            {result.error}
        ));
        emit missingLocationReviewChanged();
    } else {
        // Switching or closing the review invalidates the page while its
        // worker remains cooperative and bounded.
        emit missingLocationReviewChanged();
    }

    if (missing_locations_refresh_pending_ && !missing_location_scan_id_.isEmpty()) {
        missing_locations_refresh_pending_ = false;
        startMissingLocationTask(false);
    }
}

void ReviewSourceHealthCoordinator::finishRelinkTask() {
    RelinkTaskResult result = relink_watcher_.result();
    relink_running_ = false;
    const bool accepted = result.request_id == active_relink_request_id_;
    if (accepted && result.error.isEmpty()) {
        relink_status_message_ = source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Verified and added Library folder · %1"),
            {result.receipt.library_root_path}
        );
        publishGlobalStatus(relink_status_message_);
        if (!result.receipt.library_root_path.isEmpty()) {
            emit libraryFolderScanRequested(result.receipt.library_root_path);
        }
        emit libraryVisibilityChanged();
    } else if (accepted) {
        relink_status_message_ = source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not link selected source · %1"),
            {result.error}
        );
        publishGlobalStatus(relink_status_message_);
    }
    emit missingLocationReviewChanged();
}

void ReviewSourceHealthCoordinator::finishRecoverSourceTask() {
    RecoverSourceTaskResult result = recover_source_watcher_.result();
    recover_source_running_ = false;
    const bool accepted = result.request_id == active_recover_source_request_id_;
    if (accepted && result.error.isEmpty()) {
        relink_status_message_ = source_health_message(
            result.receipt.unresolved_photo_count == 0
                ? QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "Located %1 photos and added Library folder · %2"
                  )
                : QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "Located %1 photos; %2 remain missing · %3"
                  ),
            result.receipt.unresolved_photo_count == 0
                ? std::initializer_list<LocalizedUiArgument>{
                      result.receipt.recovered_photo_count,
                      result.receipt.library_root_path,
                  }
                : std::initializer_list<LocalizedUiArgument>{
                      result.receipt.recovered_photo_count,
                      result.receipt.unresolved_photo_count,
                      result.receipt.library_root_path,
                  }
        );
        publishGlobalStatus(relink_status_message_);
        if (!result.receipt.library_root_path.isEmpty()) {
            emit libraryFolderScanRequested(result.receipt.library_root_path);
        }
        emit libraryVisibilityChanged();
        refreshSourceHealth();
    } else if (accepted) {
        relink_status_message_ = source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not locate missing originals · %1"),
            {result.error}
        );
        publishGlobalStatus(relink_status_message_);
    }
    emit missingLocationReviewChanged();
    emit sourceHealthChanged();
}

void ReviewSourceHealthCoordinator::finishReconcileTask() {
    ReconcileTaskResult result = reconcile_watcher_.result();
    reconcile_running_ = false;
    const bool accepted = result.request_id == active_reconcile_request_id_;
    if (accepted && result.error.isEmpty()) {
        if (result.receipt.archived > 0) {
            emit libraryVisibilityChanged();
        }
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Removed %1 unavailable photos from Library · %2"
            ),
            {result.receipt.archived, result.source_path}
        ));
        closeMissingLocationReview();
        refreshSourceHealth();
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not reconcile missing photos · %1"),
            {result.error}
        ));
    }
    emit sourceHealthChanged();
}

void ReviewSourceHealthCoordinator::finishArchivePhotoTask() {
    ArchivePhotoTaskResult result = archive_photo_watcher_.result();
    archive_photo_running_ = false;
    const bool accepted = result.request_id == active_archive_photo_request_id_;
    if (accepted && result.error.isEmpty()) {
        if (result.archived) {
            emit libraryVisibilityChanged();
        }
        publishGlobalStatus(source_health_message(
            result.archived
                ? QT_TRANSLATE_NOOP("ReviewController", "Removed photo from Library · %1")
                : QT_TRANSLATE_NOOP(
                      "ReviewController",
                      "Photo was already removed from Library · %1"
                  ),
            {result.title}
        ));
    } else if (accepted) {
        publishGlobalStatus(source_health_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not remove photo from Library · %1"),
            {result.error}
        ));
    }
}

void ReviewSourceHealthCoordinator::publishGlobalStatus(LocalizedUiMessage status) {
    global_status_message_ = std::move(status);
    emit globalStatusMessageChanged();
}

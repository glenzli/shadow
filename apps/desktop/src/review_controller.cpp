#include "review_controller.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QSet>
#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t REVIEW_PAGE_SIZE = 96;
constexpr int SCAN_PROGRESS_POLL_MS = 150;
constexpr quint64 STREAM_REFRESH_STRIDE = 16;
constexpr qint64 STREAM_REFRESH_MIN_INTERVAL_MS = 400;
constexpr qint64 STREAM_VISUAL_REFRESH_MIN_INTERVAL_MS = 150;
constexpr int FILTER_QUERY_DEBOUNCE_MS = 120;
constexpr std::uint32_t MISSING_SOURCE_LOCATION_PAGE_SIZE = 24;
constexpr std::uint32_t LIBRARY_FACET_PAGE_SIZE = 24;

[[nodiscard]] LocalizedUiMessage review_message(
    const char *const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"ReviewController", source, arguments};
}

[[nodiscard]] ScanTaskResult run_scan(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& folder_path,
    const quint64 generation
) {
    ScanTaskResult result;
    result.generation = generation;
    try {
        result.report = backend->scanFolder(folder_path, generation);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] PageTaskResult run_page(
    const std::shared_ptr<DesktopBackend>& backend,
    const BackendLibraryPhotoFilter& filter,
    const BackendLibraryPhotoCursor& cursor,
    const quint64 library_generation,
    const quint64 request_id,
    const PageTaskKind kind
) {
    PageTaskResult result;
    result.library_generation = library_generation;
    result.request_id = request_id;
    result.kind = kind;
    try {
        result.page = backend->libraryPhotoPage(filter, cursor, REVIEW_PAGE_SIZE);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] CountTaskResult run_count(
    const std::shared_ptr<DesktopBackend>& backend,
    const BackendLibraryPhotoFilter& filter,
    const quint64 library_generation,
    const quint64 request_id
) {
    CountTaskResult result;
    result.library_generation = library_generation;
    result.request_id = request_id;
    try {
        result.count = backend->libraryPhotoCount(filter);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] LibraryFacetTaskResult run_library_facets(
    const std::shared_ptr<DesktopBackend>& backend,
    const BackendLibraryPhotoFilter& filter,
    const quint64 library_generation,
    const quint64 request_id
) {
    LibraryFacetTaskResult result;
    result.library_generation = library_generation;
    result.request_id = request_id;
    try {
        result.capture_months = backend->libraryFacetPage(
            filter,
            BackendLibraryFacetKind::CaptureMonth,
            {},
            LIBRARY_FACET_PAGE_SIZE
        );
        result.cameras = backend->libraryFacetPage(
            filter,
            BackendLibraryFacetKind::Camera,
            {},
            LIBRARY_FACET_PAGE_SIZE
        );
        result.lenses = backend->libraryFacetPage(
            filter,
            BackendLibraryFacetKind::Lens,
            {},
            LIBRARY_FACET_PAGE_SIZE
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] QVariantList library_facet_variants(
    const BackendLibraryFacetPage& page
) {
    QVariantList values;
    values.reserve(page.items.size());
    for (const auto& item : page.items) {
        values.push_back(QVariantMap{
            {QStringLiteral("key"), item.key},
            {QStringLiteral("label"), item.label},
            {
                QStringLiteral("photoCount"),
                QVariant::fromValue(static_cast<qulonglong>(item.photo_count)),
            },
        });
    }
    return values;
}

[[nodiscard]] LibraryStateTaskResult run_library_state_mutation(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const bool liked,
    const QString& color_label
) {
    LibraryStateTaskResult result;
    result.requested_photo_id = photo_id;
    try {
        result.state = backend->setPhotoLibraryState(
            photo_id,
            liked,
            color_label
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] LibraryAlbumTaskResult run_library_albums_task(
    const std::shared_ptr<DesktopBackend>& backend,
    const LibraryAlbumTaskAction action,
    const QString& name,
    const QString& album_id,
    const QStringList& photo_ids,
    const BackendLibraryPhotoFilter& smart_query,
    const quint64 request_id
) {
    LibraryAlbumTaskResult result;
    result.request_id = request_id;
    result.action = action;
    result.album_id = album_id;
    result.affected_photo_count = static_cast<int>(photo_ids.size());
    try {
        switch (action) {
        case LibraryAlbumTaskAction::Refresh:
            break;
        case LibraryAlbumTaskAction::CreateManual:
            static_cast<void>(backend->createManualLibraryAlbum(name));
            break;
        case LibraryAlbumTaskAction::CreateSmart:
            static_cast<void>(backend->createSmartLibraryAlbum(name, smart_query));
            break;
        case LibraryAlbumTaskAction::Rename:
            static_cast<void>(backend->renameLibraryAlbum(album_id, name));
            break;
        case LibraryAlbumTaskAction::Delete:
            static_cast<void>(backend->deleteLibraryAlbum(album_id));
            break;
        case LibraryAlbumTaskAction::AddPhotos:
            for (const QString& photo_id : photo_ids) {
                backend->addPhotoToManualLibraryAlbum(album_id, photo_id);
            }
            break;
        case LibraryAlbumTaskAction::RemovePhotos:
            for (const QString& photo_id : photo_ids) {
                static_cast<void>(backend->removePhotoFromManualLibraryAlbum(
                    album_id,
                    photo_id
                ));
            }
            break;
        }
        result.albums = backend->libraryAlbums();
        result.has_album_snapshot = true;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        try {
            result.albums = backend->libraryAlbums();
            result.has_album_snapshot = true;
        } catch (const std::exception&) {
            // Keep the primary mutation error: a follow-up refresh is best effort.
        }
    }
    return result;
}

[[nodiscard]] LibrarySourceHealthTaskResult run_library_source_health_task(
    const std::shared_ptr<DesktopBackend>& backend,
    const quint64 request_id
) {
    LibrarySourceHealthTaskResult result;
    result.request_id = request_id;
    try {
        result.sources = backend->librarySourceHealth();
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] MissingSourceLocationTaskResult run_missing_source_location_task(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& scan_session_id,
    const QString& after_location_id,
    const quint64 request_id,
    const bool append
) {
    MissingSourceLocationTaskResult result;
    result.scan_session_id = scan_session_id;
    result.request_id = request_id;
    result.append = append;
    try {
        result.page = backend->missingSourceLocationPage(
            scan_session_id,
            after_location_id,
            MISSING_SOURCE_LOCATION_PAGE_SIZE
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] QStringList photo_ids_from_targets(const QVariantList& targets) {
    QStringList photo_ids;
    QSet<QString> seen;
    for (const QVariant& value : targets) {
        const QString photo_id = value.toMap()
            .value(QStringLiteral("photoId"))
            .toString()
            .trimmed();
        if (!photo_id.isEmpty() && !seen.contains(photo_id)) {
            seen.insert(photo_id);
            photo_ids.push_back(photo_id);
        }
    }
    return photo_ids;
}

[[nodiscard]] QString decision_flag_name(const BackendReviewDecisionFlag flag) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return QStringLiteral("unflagged");
    case BackendReviewDecisionFlag::Picked:
        return QStringLiteral("picked");
    case BackendReviewDecisionFlag::Rejected:
        return QStringLiteral("rejected");
    }
    throw std::invalid_argument("unknown Review decision flag");
}

[[nodiscard]] QVector<ReviewItem> review_items(QVector<BackendReviewItem> source) {
    QVector<ReviewItem> items;
    items.reserve(source.size());
    for (auto& item : source) {
        items.push_back({
            .photo_id = std::move(item.photo_id),
            .representation_id = std::move(item.representation_id),
            .visual_handle = std::move(item.visual_handle),
            .decision_head_sequence = item.decision_head_sequence,
            .decision_flag = decision_flag_name(item.decision_flag),
            .decision_rating = static_cast<int>(item.decision_rating),
            .liked = item.liked,
            .color_label = std::move(item.color_label),
            .library_state_updated_at_ms = item.library_state_updated_at_ms,
            .has_development_edits = item.has_development_edits,
            .title = std::move(item.title),
            .source_path = std::move(item.source_path),
            .visual_role = std::move(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
            .has_metadata = item.has_metadata,
            .camera_make = std::move(item.camera_make),
            .camera_model = std::move(item.camera_model),
            .lens_make = std::move(item.lens_make),
            .lens_model = std::move(item.lens_model),
            .captured_at_unix_seconds = item.captured_at_unix_seconds,
            .iso_speed = item.iso_speed,
            .exposure_time_seconds = item.exposure_time_seconds,
            .aperture_f_number = item.aperture_f_number,
            .focal_length_mm = item.focal_length_mm,
            .focal_length_35mm = item.focal_length_35mm,
            .raw_width = item.raw_width,
            .raw_height = item.raw_height,
            .sensor_bits = item.sensor_bits,
            .cfa_pattern = std::move(item.cfa_pattern),
            .dng_version = std::move(item.dng_version),
            .has_technical_observation = item.has_technical_observation,
            .technical_input_width = item.technical_input_width,
            .technical_input_height = item.technical_input_height,
            .technical_preprocessing_version = std::move(
                item.technical_preprocessing_version
            ),
            .technical_implementation_version = std::move(
                item.technical_implementation_version
            ),
            .mean_luma = item.mean_luma,
            .p01_luma = item.p01_luma,
            .p50_luma = item.p50_luma,
            .p99_luma = item.p99_luma,
            .near_black_fraction = item.near_black_fraction,
            .near_white_fraction = item.near_white_fraction,
            .laplacian_variance = item.laplacian_variance,
            .edge_energy = item.edge_energy,
        });
    }
    return items;
}

[[nodiscard]] int bounded_count(const quint64 count) {
    return static_cast<int>(std::min<quint64>(
        count,
        static_cast<quint64>(std::numeric_limits<int>::max())
    ));
}

[[nodiscard]] std::optional<BackendPairwiseOutcome> pairwise_outcome(
    const int outcome
) {
    switch (outcome) {
    case 0:
        return BackendPairwiseOutcome::LeftPreferred;
    case 1:
        return BackendPairwiseOutcome::RightPreferred;
    case 2:
        return BackendPairwiseOutcome::KeepBoth;
    case 3:
        return BackendPairwiseOutcome::KeepNeither;
    case 4:
        return BackendPairwiseOutcome::CannotCompare;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] ReviewEvidenceTaskResult record_comparison(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& presentation_id,
    const BackendPairwiseOutcome outcome
) {
    ReviewEvidenceTaskResult result;
    result.kind = ReviewEvidenceTaskKind::Record;
    try {
        result.feedback = backend->recordReviewComparison(
            presentation_id,
            outcome
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] ReviewEvidenceTaskResult forget_comparison(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& event_id
) {
    ReviewEvidenceTaskResult result;
    result.kind = ReviewEvidenceTaskKind::Forget;
    try {
        result.forget = backend->forgetReviewFeedback(event_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] std::optional<BackendReviewDecisionFlag> decision_flag(
    const QString& flag
) {
    if (flag == QStringLiteral("unflagged")) {
        return BackendReviewDecisionFlag::Unflagged;
    }
    if (flag == QStringLiteral("picked")) {
        return BackendReviewDecisionFlag::Picked;
    }
    if (flag == QStringLiteral("rejected")) {
        return BackendReviewDecisionFlag::Rejected;
    }
    return std::nullopt;
}

[[nodiscard]] BackendReviewDecisionState backend_decision_state(
    const QString& photo_id,
    const ReviewDecisionValue& value
) {
    const auto flag = decision_flag(value.flag);
    if (!flag || value.rating < 0 || value.rating > 5) {
        throw std::invalid_argument("Review model contains an invalid decision state");
    }
    return {
        .photo_id = photo_id,
        .head_sequence = value.head_sequence,
        .flag = *flag,
        .rating = static_cast<std::uint8_t>(value.rating),
    };
}

[[nodiscard]] ReviewDecisionTaskResult run_decision_mutation(
    const std::shared_ptr<DesktopBackend>& backend,
    const ReviewDecisionMutationRequest& request
) {
    ReviewDecisionTaskResult result;
    result.is_undo = request.is_undo;
    try {
        result.receipt = backend->setReviewPhotoDecision(
            request.photo_id,
            request.expected_head_sequence,
            request.desired_flag,
            request.desired_rating
        );
        return result;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    try {
        result.authoritative = backend->reviewPhotoDecisionState(request.photo_id);
        result.has_authoritative = true;
    } catch (const std::exception& error) {
        result.refresh_error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

ReviewController::ReviewController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      model_(this),
      filtered_model_(this) {
    // Keep the constructor shape for existing test/application call sites.
    // Library state is Catalog-backed now, so the former desktop-local
    // settings file is deliberately not consulted.
    (void)isolated_settings_file;
    filtered_model_.setSourceModel(&model_);
    connect(
        &filtered_model_,
        &ReviewFilterModel::filtersChanged,
        this,
        [this]() {
            scheduleFilterQuery();
            emit filtersChanged();
        }
    );
    const auto notify_filtered_count = [this]() { emit filtersChanged(); };
    connect(&filtered_model_, &QAbstractItemModel::modelReset,
            this, notify_filtered_count);
    connect(&filtered_model_, &QAbstractItemModel::rowsInserted,
            this, [notify_filtered_count](const QModelIndex&, const int, const int) {
                notify_filtered_count();
            });
    connect(&filtered_model_, &QAbstractItemModel::rowsRemoved,
            this, [notify_filtered_count](const QModelIndex&, const int, const int) {
                notify_filtered_count();
            });
    model_.replace({}, library_generation_);
    scan_progress_timer_.setInterval(SCAN_PROGRESS_POLL_MS);
    scan_progress_timer_.setTimerType(Qt::CoarseTimer);
    filter_debounce_timer_.setInterval(FILTER_QUERY_DEBOUNCE_MS);
    filter_debounce_timer_.setSingleShot(true);
    filter_debounce_timer_.setTimerType(Qt::CoarseTimer);
    connect(
        &scan_watcher_,
        &QFutureWatcher<ScanTaskResult>::finished,
        this,
        &ReviewController::finishScan
    );
    connect(
        &page_watcher_,
        &QFutureWatcher<PageTaskResult>::finished,
        this,
        &ReviewController::finishPage
    );
    connect(
        &count_watcher_,
        &QFutureWatcher<CountTaskResult>::finished,
        this,
        &ReviewController::finishCount
    );
    connect(
        &library_facets_watcher_,
        &QFutureWatcher<LibraryFacetTaskResult>::finished,
        this,
        &ReviewController::finishLibraryFacetsTask
    );
    connect(
        &library_state_watcher_,
        &QFutureWatcher<LibraryStateTaskResult>::finished,
        this,
        &ReviewController::finishLibraryStateTask
    );
    connect(
        &library_albums_watcher_,
        &QFutureWatcher<LibraryAlbumTaskResult>::finished,
        this,
        &ReviewController::finishLibraryAlbumsTask
    );
    connect(
        &library_source_health_watcher_,
        &QFutureWatcher<LibrarySourceHealthTaskResult>::finished,
        this,
        &ReviewController::finishLibrarySourceHealthTask
    );
    connect(
        &missing_source_locations_watcher_,
        &QFutureWatcher<MissingSourceLocationTaskResult>::finished,
        this,
        &ReviewController::finishMissingSourceLocationTask
    );
    connect(
        &evidence_watcher_,
        &QFutureWatcher<ReviewEvidenceTaskResult>::finished,
        this,
        &ReviewController::finishEvidenceTask
    );
    connect(
        &decision_watcher_,
        &QFutureWatcher<ReviewDecisionTaskResult>::finished,
        this,
        &ReviewController::finishDecisionTask
    );
    connect(
        &scan_progress_timer_,
        &QTimer::timeout,
        this,
        &ReviewController::pollScanProgress
    );
    connect(
        &filter_debounce_timer_,
        &QTimer::timeout,
        this,
        &ReviewController::beginFilteredLibraryQuery
    );
  if (auto *const application = QCoreApplication::instance()) {
    application->installEventFilter(this);
  }
    QTimer::singleShot(0, this, [this]() {
        refreshSharedGradeNodes();
        refreshLibraryAlbums();
        refreshLibrarySourceHealth();
        beginFilteredLibraryQuery();
    });
}

ReviewController::~ReviewController() {
    scan_progress_timer_.stop();
    filter_debounce_timer_.stop();
    if (scan_running_) {
        try {
            static_cast<void>(backend_->cancelFolderScan(scan_generation_));
        } catch (const std::exception&) {
        }
    }
    scan_watcher_.waitForFinished();
    page_watcher_.waitForFinished();
    count_watcher_.waitForFinished();
    library_facets_watcher_.waitForFinished();
    library_state_watcher_.waitForFinished();
    library_albums_watcher_.waitForFinished();
    library_source_health_watcher_.waitForFinished();
    missing_source_locations_watcher_.waitForFinished();
    evidence_watcher_.waitForFinished();
    decision_watcher_.waitForFinished();
}

bool ReviewController::busy() const noexcept {
    return model_.rowCount() == 0
        && (scan_running_ || page_running_ || terminal_refresh_active_);
}

bool ReviewController::scanning() const noexcept {
    return scan_running_;
}

bool ReviewController::refreshing() const noexcept {
    return page_reset_running_ || terminal_refresh_active_;
}

bool ReviewController::loadingMore() const noexcept {
    return page_running_ && !page_reset_running_ && model_.rowCount() > 0;
}

bool ReviewController::hasMore() const noexcept {
    return has_more_;
}

QString ReviewController::folderPath() const {
    return folder_path_;
}

QString ReviewController::statusText() const {
    return status_message_.translated();
}

QVariantMap ReviewController::scanProgress() const {
    const quint64 catalogued = inserted_files_ + unchanged_files_ + revalidation_files_;
    QString phase = QStringLiteral("idle");
    switch (scan_phase_) {
    case BackendScanPhase::Idle:
        break;
    case BackendScanPhase::Discovering:
        phase = QStringLiteral("discovering");
        break;
    case BackendScanPhase::PreparingPreviews:
        phase = QStringLiteral("preparing-previews");
        break;
    case BackendScanPhase::Cancelling:
        phase = QStringLiteral("cancelling");
        break;
    case BackendScanPhase::Completed:
        phase = QStringLiteral("completed");
        break;
    case BackendScanPhase::Cancelled:
        phase = QStringLiteral("cancelled");
        break;
    case BackendScanPhase::Failed:
        phase = QStringLiteral("failed");
        break;
    }
    return {
        {QStringLiteral("scanId"), QVariant::fromValue(scan_generation_)},
        {QStringLiteral("updateSequence"), QVariant::fromValue(scan_update_sequence_)},
        {QStringLiteral("phase"), phase},
        {QStringLiteral("filesSeen"), QVariant::fromValue(files_seen_)},
        {QStringLiteral("supportedFiles"), QVariant::fromValue(supported_files_)},
        {QStringLiteral("cataloguedFiles"), QVariant::fromValue(catalogued)},
        {QStringLiteral("insertedFiles"), QVariant::fromValue(inserted_files_)},
        {QStringLiteral("unchangedFiles"), QVariant::fromValue(unchanged_files_)},
        {QStringLiteral("revalidationFiles"), QVariant::fromValue(revalidation_files_)},
        {QStringLiteral("decodeQueued"), QVariant::fromValue(decode_queued_)},
        {QStringLiteral("previewArtifactsReady"), QVariant::fromValue(preview_artifacts_ready_)},
        {QStringLiteral("decodeCompleted"), QVariant::fromValue(decode_completed_)},
        {
            QStringLiteral("decodeHardFailures"),
            QVariant::fromValue(decode_hard_failures_),
        },
        {QStringLiteral("previewFailures"), QVariant::fromValue(preview_failures_)},
        {QStringLiteral("decodeCancelled"), QVariant::fromValue(decode_cancelled_)},
        {QStringLiteral("skippedFiles"), QVariant::fromValue(skipped_files_)},
        {QStringLiteral("issueCount"), QVariant::fromValue(issue_count_)},
        {QStringLiteral("targetPath"), folder_path_},
        {QStringLiteral("cancellable"), scan_running_},
    };
}

int ReviewController::itemCount() const {
    return bounded_count(total_items_);
}

bool ReviewController::comparisonBusy() const noexcept {
    return evidence_session_.busy();
}

bool ReviewController::canUndoComparison() const noexcept {
    return evidence_session_.canForget();
}

int ReviewController::sessionEvidenceCount() const noexcept {
    return evidence_session_.activeCount();
}

QString ReviewController::comparisonStatusText() const {
    return comparison_status_message_.translated();
}

bool ReviewController::decisionBusy() const noexcept {
    return decision_session_.busy();
}

bool ReviewController::canUndoDecision() const {
    return decision_session_.canUndo();
}

QString ReviewController::decisionStatusText() const {
    return decision_status_message_.translated();
}

QString ReviewController::filterFlag() const {
    return filtered_model_.flagFilter();
}

int ReviewController::filterMinimumRating() const noexcept {
    return filtered_model_.minimumRating();
}

QString ReviewController::filterColorLabel() const {
    return filtered_model_.colorFilter();
}

QString ReviewController::filterEditState() const {
    return filtered_model_.editFilter();
}

QString ReviewController::filterCaptureMonth() const {
    return filtered_model_.captureMonth();
}

QString ReviewController::filterCameraKey() const {
    return filtered_model_.cameraKey();
}

QString ReviewController::filterLensKey() const {
    return filtered_model_.lensKey();
}

QVariantList ReviewController::libraryCaptureMonthFacets() const {
    return library_facet_variants(library_capture_month_facets_);
}

QVariantList ReviewController::libraryCameraFacets() const {
    return library_facet_variants(library_camera_facets_);
}

QVariantList ReviewController::libraryLensFacets() const {
    return library_facet_variants(library_lens_facets_);
}

bool ReviewController::libraryFacetsBusy() const noexcept {
    return library_facets_task_running_;
}

QString ReviewController::libraryAlbumId() const {
    return library_album_id_;
}

QVariantList ReviewController::libraryAlbums() const {
    QVariantList result;
    result.reserve(library_albums_.size());
    for (const auto& album : library_albums_) {
        result.push_back(QVariantMap{
            {QStringLiteral("id"), album.id},
            {QStringLiteral("name"), album.name},
            {
                QStringLiteral("kind"),
                album.kind == BackendLibraryAlbumKind::Smart
                    ? QStringLiteral("smart") : QStringLiteral("manual"),
            },
        });
    }
    return result;
}

bool ReviewController::libraryAlbumsBusy() const noexcept {
    return library_albums_task_running_;
}

QVariantList ReviewController::librarySourceHealth() const {
    QVariantList result;
    result.reserve(library_source_health_.size());
    for (const auto& source : library_source_health_) {
        result.push_back(QVariantMap{
            {QStringLiteral("sourceId"), source.source_id},
            {QStringLiteral("sourcePath"), source.source_display_path},
            {QStringLiteral("enabled"), source.source_enabled},
            {QStringLiteral("hasLatestCompletedScan"), source.has_latest_completed_scan},
            {QStringLiteral("scanSessionId"), source.scan_session_id},
            {QStringLiteral("scanCompletedAtMs"), source.scan_completed_at_ms},
            {QStringLiteral("knownLocations"), source.known_locations},
            {QStringLiteral("seenLocations"), source.seen_locations},
            {QStringLiteral("notSeenLocations"), source.not_seen_locations},
        });
    }
    return result;
}

bool ReviewController::librarySourceHealthBusy() const noexcept {
    return library_source_health_task_running_;
}

QVariantList ReviewController::missingSourceLocations() const {
    QVariantList result;
    result.reserve(missing_source_locations_.size());
    for (const auto& location : missing_source_locations_) {
        result.push_back(QVariantMap{
            {QStringLiteral("locationId"), location.location_id},
            {QStringLiteral("photoId"), location.photo_id},
            {QStringLiteral("title"), location.title},
            {QStringLiteral("sourcePath"), location.source_display_path},
            {QStringLiteral("hasCapturedAt"), location.has_captured_at},
            {QStringLiteral("capturedAtUnixSeconds"), location.captured_at_unix_seconds},
            {QStringLiteral("cameraKey"), location.camera_key},
            {QStringLiteral("lastSeenAtMs"), location.last_seen_at_ms},
        });
    }
    return result;
}

QString ReviewController::missingSourceLocationScanId() const {
    return missing_source_location_scan_id_;
}

bool ReviewController::missingSourceLocationsBusy() const noexcept {
    return missing_source_locations_task_running_;
}

bool ReviewController::missingSourceLocationsHasMore() const noexcept {
    return missing_source_locations_has_more_;
}

int ReviewController::filteredItemCount() const noexcept {
    return bounded_count(total_items_);
}

QVariantList ReviewController::sharedGradeNodes() const {
    QVariantList result;
    result.reserve(shared_grade_nodes_.size());
    for (const auto& shared : shared_grade_nodes_) {
        result.push_back(QVariantMap{
            {QStringLiteral("layerId"), shared.layer_id},
            {QStringLiteral("revisionId"), shared.revision_id},
            {QStringLiteral("revisionNumber"), shared.revision_number},
            {QStringLiteral("label"), shared.label},
        });
    }
    return result;
}

QAbstractItemModel* ReviewController::model() noexcept {
    return &filtered_model_;
}

ReviewModel* ReviewController::reviewModel() noexcept {
    return &model_;
}

void ReviewController::scanFolder(const QUrl& folder_url) {
    if (scan_running_ || page_running_ || terminal_refresh_active_
        || evidence_session_.busy() || decision_session_.busy()) {
        return;
    }
    const QString path = folder_url.toLocalFile();
    if (path.isEmpty()) {
    setStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "The selected folder is not a local path")));
        return;
    }

    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    ++scan_generation_;
    if (scan_generation_ == 0) {
        ++scan_generation_;
    }
    try {
        backend_->beginFolderScan(scan_generation_);
    } catch (const std::exception& error) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Could not start import · %1"),
        {QString::fromUtf8(error.what())})
        );
        return;
    }
    scan_update_sequence_ = 0;
    files_seen_ = 0;
    supported_files_ = 0;
    inserted_files_ = 0;
    unchanged_files_ = 0;
    revalidation_files_ = 0;
    decode_queued_ = 0;
    preview_artifacts_ready_ = 0;
    decode_completed_ = 0;
    decode_hard_failures_ = 0;
    preview_failures_ = 0;
    decode_cancelled_ = 0;
    skipped_files_ = 0;
    issue_count_ = 0;
    next_stream_refresh_at_ = 1;
    last_stream_visual_refresh_at_ = 0;
    last_stream_refresh_ms_ = -1;
    scan_phase_ = BackendScanPhase::Discovering;
    scan_terminal_error_.clear();
    scan_terminal_cancelled_ = false;
    library_reset_pending_ = false;
    terminal_refresh_active_ = false;
    setHasMore(false);
    folder_path_ = path;
    emit folderPathChanged();
    scan_running_ = true;
    scan_clock_.restart();
    scan_progress_timer_.start();
    emit scanningChanged();
    emit scanProgressChanged();
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    updateScanStatus();
    scan_watcher_.setFuture(QtConcurrent::run(
        [backend = backend_, folder = folder_path_, generation = scan_generation_]() {
            return run_scan(backend, folder, generation);
        }
    ));
}

void ReviewController::cancelScan() {
    if (!scan_running_ || scan_phase_ == BackendScanPhase::Cancelling) {
        return;
    }
    try {
        if (!backend_->cancelFolderScan(scan_generation_)) {
            return;
        }
    } catch (const std::exception& error) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Could not cancel import · %1"),
        {QString::fromUtf8(error.what())})
        );
        return;
    }
    scan_phase_ = BackendScanPhase::Cancelling;
    emit scanProgressChanged();
    updateScanStatus();
}

void ReviewController::loadMore() {
    if (!has_more_ || scan_running_ || refreshing() || page_running_
        || evidence_session_.busy() || decision_session_.busy()) {
        return;
    }
    startPage(PageTaskKind::Append);
}

QVariantMap ReviewController::prepareComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) {
    if (evidence_session_.busy() || decision_session_.busy() || scan_running_
        || refreshing() || page_running_) {
        return {};
    }
    if (left_visual_handle.trimmed().isEmpty()
        || right_visual_handle.trimmed().isEmpty()) {
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Choose two verified visuals before comparing")));
        return {};
    }
    try {
        const auto presentation = backend_->prepareReviewComparison(
            left_visual_handle,
            right_visual_handle
        );
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Loading two exact Compare frames with durable provenance…"))
        );
        return {
            {QStringLiteral("presentationId"), presentation.presentation_id},
            {QStringLiteral("leftRequestTicket"), presentation.left_request_ticket},
            {QStringLiteral("rightRequestTicket"), presentation.right_request_ticket},
            {
                QStringLiteral("leftSource"),
                model_.visualSourceFor(presentation.left_request_ticket),
            },
            {
                QStringLiteral("rightSource"),
                model_.visualSourceFor(presentation.right_request_ticket),
            },
        };
    } catch (const std::exception& error) {
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Cannot prepare exact comparison · %1"),
        {QString::fromUtf8(error.what())})
        );
        return {};
    }
}

bool ReviewController::confirmComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) {
    if (decision_session_.busy()) {
        return false;
    }
    if (presentation_id.trimmed().isEmpty()
        || left_request_ticket.trimmed().isEmpty()
        || right_request_ticket.trimmed().isEmpty()) {
        return false;
    }
    try {
        backend_->confirmReviewComparisonReady(
            presentation_id,
            left_request_ticket,
            right_request_ticket
        );
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Exact encoded artifacts and decoded Compare frames verified"))
        );
        return true;
    } catch (const std::exception& error) {
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Comparison frame verification failed · %1"),
        {QString::fromUtf8(error.what())})
        );
        return false;
    }
}

void ReviewController::cancelComparison(const QString& presentation_id) {
    if (presentation_id.trimmed().isEmpty() || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    try {
        backend_->cancelReviewComparison(presentation_id);
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Comparison presentation closed")));
    } catch (const std::exception& error) {
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Cannot close comparison presentation · %1"),
        {QString::fromUtf8(error.what())})
        );
    }
}

void ReviewController::recordComparison(
    const QString& presentation_id,
    const int outcome
) {
    const auto resolved_outcome = pairwise_outcome(outcome);
    if (!resolved_outcome) {
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Comparison outcome is not supported")));
        return;
    }
    if (presentation_id.trimmed().isEmpty()) {
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Prepare and verify the comparison first")));
        return;
    }
    if (decision_session_.busy() || scan_running_ || refreshing() || page_running_) {
        return;
    }
    if (!evidence_session_.beginRecord()) {
        return;
    }
    emit comparisonStateChanged();
  setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Recording append-only comparison evidence…")));
    evidence_watcher_.setFuture(QtConcurrent::run(
        record_comparison,
        backend_,
        presentation_id,
        *resolved_outcome
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_session_.busy() || scan_running_ || refreshing() || page_running_) {
        return;
    }
    const auto event_id = evidence_session_.beginForget();
    if (!event_id) {
        return;
    }
    emit comparisonStateChanged();
  setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending a forget fact for the latest evidence…")));
    evidence_watcher_.setFuture(QtConcurrent::run(
        forget_comparison,
        backend_,
        *event_id
    ));
}

void ReviewController::setPhotoFlag(
    const QString& photo_id,
    const QString& flag
) {
    const auto desired_flag = decision_flag(flag);
    if (!desired_flag) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Unsupported Review flag")));
        return;
    }
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current_value = model_.decisionFor(photo_id);
    if (!current_value) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Select a loaded photo before setting a flag")));
        return;
    }
    BackendReviewDecisionState current;
    try {
        current = backend_decision_state(photo_id, *current_value);
    } catch (const std::exception& error) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Could not read the current flag decision · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    const auto request = decision_session_.beginSet(
        std::move(current),
        *desired_flag,
        static_cast<std::uint8_t>(current_value->rating)
    );
    if (!request) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Flag already matches the selected photo")));
        return;
    }
  setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending an explicit flag decision…")));
    startDecisionMutation(*request);
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    if (rating < 0 || rating > 5) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Rating must be between 0 and 5 stars")));
        return;
    }
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current_value = model_.decisionFor(photo_id);
    if (!current_value) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Select a loaded photo before setting a rating")));
        return;
    }
    BackendReviewDecisionState current;
    try {
        current = backend_decision_state(photo_id, *current_value);
    } catch (const std::exception& error) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Could not read the current star rating · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    const BackendReviewDecisionFlag current_flag = current.flag;
    const auto request = decision_session_.beginSet(
        std::move(current),
        current_flag,
        static_cast<std::uint8_t>(rating)
    );
    if (!request) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Rating already matches the selected photo")));
        return;
    }
  setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending an explicit star rating…")));
    startDecisionMutation(*request);
}

void ReviewController::setPhotoColorLabel(
    const QString& photo_id,
    const QString& color_label
) {
    if (library_state_mutation_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const QString normalized = color_label.trimmed().toLower();
    const auto current = model_.libraryStateFor(photo_id);
    if (!current) {
        setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
            "ReviewController", "Select a loaded photo before changing its color label"
        )));
        return;
    }
    if (current->color_label == normalized) {
        return;
    }
    startLibraryStateMutation(photo_id, current->liked, normalized);
}

void ReviewController::setPhotoLiked(const QString& photo_id, const bool liked) {
    if (library_state_mutation_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current = model_.libraryStateFor(photo_id);
    if (!current) {
        setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
            "ReviewController", "Select a loaded photo before changing its Like state"
        )));
        return;
    }
    if (current->liked == liked) {
        return;
    }
    startLibraryStateMutation(photo_id, liked, current->color_label);
}

void ReviewController::clearFilters() {
    filtered_model_.clearFilters();
    if (!library_album_id_.isEmpty()) {
        library_album_id_.clear();
        emit libraryAlbumChanged();
        scheduleFilterQuery();
    }
}

void ReviewController::refreshVisibleLibrary() {
    if (scan_running_ || decision_session_.busy()) {
        return;
    }
    requestLibraryReset();
}

void ReviewController::refreshLibraryFacets() {
    if (!scan_running_) {
        startLibraryFacetsTask();
    }
}

void ReviewController::setLibraryFacet(const QString& kind, const QString& key) {
    const QString normalized_kind = kind.trimmed().toLower();
    if (normalized_kind == QStringLiteral("month")) {
        setFilterCaptureMonth(key);
    } else if (normalized_kind == QStringLiteral("camera")) {
        setFilterCameraKey(key);
    } else if (normalized_kind == QStringLiteral("lens")) {
        setFilterLensKey(key);
    }
}

void ReviewController::clearLibraryFacet(const QString& kind) {
    setLibraryFacet(kind, {});
}

void ReviewController::refreshLibraryAlbums() {
    if (library_albums_task_running_) {
        library_albums_refresh_pending_ = true;
        return;
    }
    startLibraryAlbumsTask(LibraryAlbumTaskAction::Refresh);
}

void ReviewController::refreshLibrarySourceHealth() {
    if (library_source_health_task_running_) {
        library_source_health_refresh_pending_ = true;
        return;
    }
    startLibrarySourceHealthTask();
}

void ReviewController::openMissingSourceLocationReview(const QString& scan_session_id) {
    const QString normalized_scan_id = scan_session_id.trimmed();
    if (normalized_scan_id.isEmpty()) {
        return;
    }
    missing_source_location_scan_id_ = normalized_scan_id;
    missing_source_location_next_cursor_.clear();
    missing_source_locations_.clear();
    missing_source_locations_has_more_ = false;
    if (missing_source_locations_task_running_) {
        active_missing_source_locations_request_id_ = ++missing_source_locations_request_id_;
        missing_source_locations_refresh_pending_ = true;
        emit missingSourceLocationReviewChanged();
        return;
    }
    startMissingSourceLocationTask(false);
}

void ReviewController::closeMissingSourceLocationReview() {
    active_missing_source_locations_request_id_ = ++missing_source_locations_request_id_;
    missing_source_locations_refresh_pending_ = false;
    missing_source_location_scan_id_.clear();
    missing_source_location_next_cursor_.clear();
    missing_source_locations_.clear();
    missing_source_locations_has_more_ = false;
    emit missingSourceLocationReviewChanged();
}

void ReviewController::loadMoreMissingSourceLocations() {
    if (missing_source_locations_task_running_ || !missing_source_locations_has_more_
        || missing_source_location_scan_id_.isEmpty()) {
        return;
    }
    startMissingSourceLocationTask(true);
}

void ReviewController::createManualLibraryAlbum(const QString& name) {
    if (library_albums_task_running_ || name.trimmed().isEmpty()) {
        return;
    }
    startLibraryAlbumsTask(LibraryAlbumTaskAction::CreateManual, name.trimmed());
}

void ReviewController::createSmartLibraryAlbum(const QString& name) {
    if (library_albums_task_running_ || name.trimmed().isEmpty()) {
        return;
    }
    startLibraryAlbumsTask(LibraryAlbumTaskAction::CreateSmart, name.trimmed());
}

void ReviewController::renameLibraryAlbum(
    const QString& album_id,
    const QString& name
) {
    const QString normalized_album_id = album_id.trimmed();
    const QString normalized_name = name.trimmed();
    if (library_albums_task_running_ || normalized_album_id.isEmpty()
        || normalized_name.isEmpty()) {
        return;
    }
    startLibraryAlbumsTask(
        LibraryAlbumTaskAction::Rename,
        normalized_name,
        normalized_album_id
    );
}

void ReviewController::deleteLibraryAlbum(const QString& album_id) {
    const QString normalized_album_id = album_id.trimmed();
    if (library_albums_task_running_ || normalized_album_id.isEmpty()) {
        return;
    }
    startLibraryAlbumsTask(
        LibraryAlbumTaskAction::Delete,
        {},
        normalized_album_id
    );
}

void ReviewController::addPhotosToManualLibraryAlbum(
    const QString& album_id,
    const QVariantList& targets
) {
    const QString normalized_album_id = album_id.trimmed();
    const QStringList photo_ids = photo_ids_from_targets(targets);
    if (library_albums_task_running_ || normalized_album_id.isEmpty()
        || photo_ids.isEmpty()) {
        return;
    }
    startLibraryAlbumsTask(
        LibraryAlbumTaskAction::AddPhotos,
        {},
        normalized_album_id,
        photo_ids
    );
}

void ReviewController::removePhotosFromManualLibraryAlbum(
    const QString& album_id,
    const QVariantList& targets
) {
    const QString normalized_album_id = album_id.trimmed();
    const QStringList photo_ids = photo_ids_from_targets(targets);
    if (library_albums_task_running_ || normalized_album_id.isEmpty()
        || photo_ids.isEmpty()) {
        return;
    }
    startLibraryAlbumsTask(
        LibraryAlbumTaskAction::RemovePhotos,
        {},
        normalized_album_id,
        photo_ids
    );
}

void ReviewController::refreshSharedGradeNodes() {
    try {
        const auto refreshed = backend_->sharedGradeNodes();
        if (refreshed != shared_grade_nodes_) {
            shared_grade_nodes_ = refreshed;
            emit sharedGradeNodesChanged();
        }
    } catch (const std::exception& error) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController",
                              "Could not load shared Grade Nodes · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

QVariantMap ReviewController::applySharedGradeNode(
    const QString& layer_id,
    const QVariantList& targets
) {
    QVector<BackendBatchPhotoTarget> batch;
    batch.reserve(targets.size());
    QSet<QString> seen_photo_ids;
    for (const auto& value : targets) {
        const auto target = value.toMap();
        const QString photo_id = target.value(QStringLiteral("photoId")).toString();
        const QString source_path =
            target.value(QStringLiteral("sourcePath")).toString();
        if (photo_id.isEmpty() || source_path.isEmpty()
            || seen_photo_ids.contains(photo_id)) {
            continue;
        }
        seen_photo_ids.insert(photo_id);
        batch.push_back({
            .photo_id = photo_id,
            .source_path = source_path,
        });
    }
    if (layer_id.isEmpty() || batch.isEmpty()) {
        return {
            {QStringLiteral("requested"), 0},
            {QStringLiteral("updated"), 0},
            {QStringLiteral("unchanged"), 0},
            {QStringLiteral("failed"), 0},
            {QStringLiteral("errors"), QStringList{}},
        };
    }
    try {
        const auto receipt =
            backend_->applySharedGradeNodeToPhotos(layer_id, batch);
        QStringList errors;
        errors.reserve(receipt.errors.size());
        for (const auto& error : receipt.errors) {
            errors.push_back(error);
        }
        if (receipt.failed == 0) {
            setStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Shared Grade Node linked to %1 photos · %2 already current"
                ),
                {
                    static_cast<qulonglong>(receipt.updated),
                    static_cast<qulonglong>(receipt.unchanged),
                }
            ));
        } else {
            setStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Shared Grade Node linked to %1 photos · %2 failed"
                ),
                {
                    static_cast<qulonglong>(receipt.updated),
                    static_cast<qulonglong>(receipt.failed),
                }
            ));
        }
        if (receipt.updated > 0) {
            refreshVisibleLibrary();
        }
        return {
            {QStringLiteral("requested"), receipt.requested},
            {QStringLiteral("updated"), receipt.updated},
            {QStringLiteral("unchanged"), receipt.unchanged},
            {QStringLiteral("failed"), receipt.failed},
            {QStringLiteral("errors"), errors},
        };
    } catch (const std::exception& error) {
        const QString message = QString::fromUtf8(error.what());
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not apply shared Grade Node · %1"
            ),
            {message}
        ));
        return {
            {QStringLiteral("requested"), batch.size()},
            {QStringLiteral("updated"), 0},
            {QStringLiteral("unchanged"), 0},
            {QStringLiteral("failed"), batch.size()},
            {QStringLiteral("errors"), QStringList{message}},
        };
    }
}

void ReviewController::setFilterFlag(const QString& filter) {
    filtered_model_.setFlagFilter(filter);
}

void ReviewController::setFilterMinimumRating(const int rating) {
    filtered_model_.setMinimumRating(rating);
}

void ReviewController::setFilterColorLabel(const QString& color_label) {
    filtered_model_.setColorFilter(color_label);
}

void ReviewController::setFilterEditState(const QString& edit_state) {
    filtered_model_.setEditFilter(edit_state);
}

void ReviewController::setFilterCaptureMonth(const QString& capture_month) {
    filtered_model_.setCaptureMonth(capture_month);
}

void ReviewController::setFilterCameraKey(const QString& camera_key) {
    filtered_model_.setCameraKey(camera_key);
}

void ReviewController::setFilterLensKey(const QString& lens_key) {
    filtered_model_.setLensKey(lens_key);
}

void ReviewController::setLibraryAlbumId(const QString& album_id) {
    const QString normalized = album_id.trimmed();
    if (library_album_id_ == normalized) {
        return;
    }
    library_album_id_ = normalized;
    emit libraryAlbumChanged();
    scheduleFilterQuery();
}

void ReviewController::undoLastDecision() {
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto request = decision_session_.beginUndo();
    if (!request) {
    setDecisionStatusMessage(review_message(
            decision_session_.undoDepth() > 0
                ? QT_TRANSLATE_NOOP("ReviewController",
                                "Local undo is disabled because the "
                                "authoritative decision changed")
                : QT_TRANSLATE_NOOP(
                  "ReviewController",
                  "No decision from this app session is available to undo"))
        );
        emit decisionStateChanged();
        return;
    }
  setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending an inverse decision event…")));
    startDecisionMutation(*request);
}

void ReviewController::startDecisionMutation(
    const ReviewDecisionMutationRequest& request
) {
    emit decisionStateChanged();
    decision_watcher_.setFuture(QtConcurrent::run(
        run_decision_mutation,
        backend_,
        request
    ));
}

void ReviewController::finishScan() {
    const ScanTaskResult result = scan_watcher_.result();
    if (result.generation != scan_generation_) {
        return;
    }
    scan_progress_timer_.stop();
    pollScanProgress();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    terminal_refresh_active_ = true;
    library_reset_pending_ = true;
    if (!result.error.isEmpty()) {
        scan_phase_ = BackendScanPhase::Failed;
        scan_terminal_error_ = result.error;
        scan_terminal_cancelled_ = false;
    } else {
        folder_path_ = result.report.folder_path;
        files_seen_ = result.report.files_seen;
        supported_files_ = result.report.supported_files;
        inserted_files_ = result.report.inserted;
        unchanged_files_ = result.report.unchanged;
        revalidation_files_ = result.report.needs_revalidation;
        decode_queued_ = result.report.decode_queued;
        decode_completed_ = result.report.decode_completed;
        decode_hard_failures_ = result.report.decode_hard_failures;
        preview_failures_ = result.report.preview_failures;
        decode_cancelled_ = result.report.decode_cancelled;
        issue_count_ = result.report.issue_count;
        scan_terminal_cancelled_ = result.report.cancelled;
        scan_phase_ = result.report.cancelled ? BackendScanPhase::Cancelled
                                              : BackendScanPhase::Completed;
        emit folderPathChanged();
    }
    scan_running_ = false;
    emit scanningChanged();
    emit scanProgressChanged();
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    refreshLibrarySourceHealth();
    requestLibraryReset();
}

void ReviewController::pollScanProgress() {
    if (scan_generation_ == 0) {
        return;
    }
    BackendScanProgress progress;
    try {
        progress = backend_->scanProgress(scan_generation_);
    } catch (const std::exception& error) {
        if (scan_running_) {
      setStatusMessage(
          review_message(QT_TRANSLATE_NOOP("ReviewController",
                                           "Import progress unavailable · %1"),
                         {QString::fromUtf8(error.what())})
            );
        }
        return;
    }
    if (!progress.valid || progress.scan_id != scan_generation_
        || progress.update_sequence <= scan_update_sequence_) {
        return;
    }

    scan_update_sequence_ = progress.update_sequence;
    files_seen_ = progress.files_seen;
    supported_files_ = progress.supported_files;
    inserted_files_ = progress.inserted;
    unchanged_files_ = progress.unchanged;
    revalidation_files_ = progress.needs_revalidation;
    decode_queued_ = progress.decode_queued;
    preview_artifacts_ready_ = progress.preview_artifacts_ready;
    decode_completed_ = progress.decode_completed;
    decode_hard_failures_ = progress.decode_hard_failures;
    preview_failures_ = progress.preview_failures;
    decode_cancelled_ = progress.decode_cancelled;
    skipped_files_ = progress.skipped;
    issue_count_ = progress.issue_count;
    scan_phase_ = progress.phase;
    emit scanProgressChanged();
    if (scan_running_) {
        updateScanStatus();
    }

    const quint64 catalogued = inserted_files_ + unchanged_files_ + revalidation_files_;
    const qint64 elapsed = scan_clock_.isValid() ? scan_clock_.elapsed() : 0;
    const bool first_visible_page = model_.rowCount() == 0 && catalogued > 0;
    const bool paced_refresh = catalogued >= next_stream_refresh_at_
        && (last_stream_refresh_ms_ < 0
            || elapsed - last_stream_refresh_ms_ >= STREAM_REFRESH_MIN_INTERVAL_MS);
    const bool visual_refresh = preview_artifacts_ready_ > last_stream_visual_refresh_at_
        && (last_stream_refresh_ms_ < 0
            || elapsed - last_stream_refresh_ms_ >= STREAM_VISUAL_REFRESH_MIN_INTERVAL_MS);
    if (scan_running_ && !page_running_
        && (first_visible_page || paced_refresh || visual_refresh)) {
        last_stream_refresh_ms_ = elapsed;
        next_stream_refresh_at_ = catalogued + STREAM_REFRESH_STRIDE;
        last_stream_visual_refresh_at_ = preview_artifacts_ready_;
        startPage(PageTaskKind::StreamingPrefix);
    }
}

void ReviewController::requestLibraryReset() {
    // An explicit refresh must supersede, rather than be followed by, a stale
    // debounced filter reset. Otherwise a manual refresh can issue two full
    // server queries back-to-back for the same filter state.
    filter_debounce_timer_.stop();
    terminal_refresh_active_ = true;
    library_reset_pending_ = true;
    if (page_running_ || decision_session_.busy()) {
        return;
    }
    beginFilteredLibraryQuery();
}

void ReviewController::scheduleFilterQuery() {
    library_reset_pending_ = true;
    filter_debounce_timer_.start();
}

void ReviewController::beginFilteredLibraryQuery() {
    if (page_running_ || decision_session_.busy()) {
        library_reset_pending_ = true;
        return;
    }

    library_reset_pending_ = false;
    ++library_generation_;
    if (library_generation_ == 0) {
        ++library_generation_;
    }
    model_.setGeneration(library_generation_);
    next_cursor_ = {};
    setHasMore(false);
    if (!scan_running_) {
        terminal_refresh_active_ = true;
    }
    startLibraryFacetsTask();
    startCountQuery();
    startPage(PageTaskKind::InitialReset);
}

void ReviewController::finishPage() {
    PageTaskResult result = page_watcher_.result();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    page_running_ = false;
    page_reset_running_ = false;
    if (result.library_generation != library_generation_
        || result.request_id != active_page_request_id_) {
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        if (library_reset_pending_ && !filter_debounce_timer_.isActive()) {
            beginFilteredLibraryQuery();
        }
        return;
    }

    const auto finish_failure = [this, &result, old_busy, old_loading_more,
                                 old_refreshing](const QString& error) {
        if (result.kind == PageTaskKind::InitialReset && !scan_running_) {
            terminal_refresh_active_ = false;
        }
        if (scan_running_) {
            setStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Live Library refresh delayed · import is still safe and continuing · %1"
                ),
                {error}
            ));
        } else {
            setStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Library refresh failed · visible photos retained · %1"
                ),
                {error}
            ));
        }
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        if (library_reset_pending_ && !filter_debounce_timer_.isActive()) {
            beginFilteredLibraryQuery();
        }
    };

    if (!result.error.isEmpty()) {
        finish_failure(result.error);
        return;
    }
    if (result.page.has_more
        && (result.page.items.isEmpty()
            || !result.page.next_cursor.has_capture_time
            || result.page.next_cursor.photo_id.isEmpty())) {
        finish_failure(QStringLiteral("the page exposed an invalid continuation cursor"));
        return;
    }

    auto items = review_items(std::move(result.page.items));
    QVector<BackendReviewDecisionState> decision_states;
    decision_states.reserve(items.size());
    try {
        for (const auto& item : items) {
            decision_states.push_back(backend_decision_state(
                item.photo_id,
                {
                    .head_sequence = item.decision_head_sequence,
                    .flag = item.decision_flag,
                    .rating = item.decision_rating,
                }
            ));
        }
    } catch (const std::exception& error) {
        finish_failure(QString::fromUtf8(error.what()));
        return;
    }

    bool accepted = false;
    switch (result.kind) {
    case PageTaskKind::InitialReset:
        accepted = model_.reconcileSnapshot(std::move(items), library_generation_);
        break;
    case PageTaskKind::StreamingPrefix:
        accepted = model_.reconcilePrefixSnapshot(
            std::move(items),
            library_generation_
        );
        break;
    case PageTaskKind::Append:
        accepted = model_.appendSnapshot(std::move(items), library_generation_);
        break;
    }
    if (!accepted) {
        finish_failure(QStringLiteral("the page contained invalid or duplicate photo ids"));
        return;
    }

    for (auto& state : decision_states) {
        decision_session_.reconcile(std::move(state));
    }
    if (result.kind == PageTaskKind::StreamingPrefix) {
        setHasMore(false);
    } else {
        next_cursor_ = std::move(result.page.next_cursor);
        setHasMore(result.page.has_more);
    }
    if (result.kind == PageTaskKind::InitialReset && !scan_running_) {
        terminal_refresh_active_ = false;
    }
    emit decisionStateChanged();
    if (library_reset_pending_ && !filter_debounce_timer_.isActive()) {
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        beginFilteredLibraryQuery();
        return;
    }
    if (scan_running_) {
        updateScanStatus();
    } else {
        updateReadyStatus();
    }
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
}

void ReviewController::finishCount() {
    const CountTaskResult result = count_watcher_.result();
    count_running_ = false;
    const bool accepted = result.library_generation == library_generation_
        && result.request_id == active_count_request_id_;
    if (accepted && result.error.isEmpty()) {
        if (total_items_ != result.count) {
            total_items_ = result.count;
            emit itemCountChanged();
            emit filtersChanged();
        }
    } else if (accepted && !result.error.isEmpty()) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not count Library photos · %1"),
            {result.error}
        ));
    }

    if (count_query_pending_ || !accepted) {
        count_query_pending_ = false;
        startCountQuery();
        return;
    }
    if (!scan_running_ && !page_running_) {
        updateReadyStatus();
    }
}

void ReviewController::finishLibraryFacetsTask() {
    const LibraryFacetTaskResult result = library_facets_watcher_.result();
    library_facets_task_running_ = false;
    const bool accepted = result.library_generation == library_generation_
        && result.request_id == active_library_facets_request_id_;
    if (accepted && result.error.isEmpty()) {
        library_capture_month_facets_ = result.capture_months;
        library_camera_facets_ = result.cameras;
        library_lens_facets_ = result.lenses;
    } else if (accepted && !result.error.isEmpty()) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not update Library facets · %1"),
            {result.error}
        ));
    }

    if (library_facets_refresh_pending_ || !accepted) {
        library_facets_refresh_pending_ = false;
        startLibraryFacetsTask();
        return;
    }
    emit libraryFacetsChanged();
}

void ReviewController::finishLibraryStateTask() {
    const LibraryStateTaskResult result = library_state_watcher_.result();
    library_state_mutation_running_ = false;
    if (!result.error.isEmpty()) {
        setDecisionStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not update Library state · %1"),
            {result.error}
        ));
        return;
    }
    if (result.state.photo_id.isEmpty()
        || result.state.photo_id != result.requested_photo_id) {
        setDecisionStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Library state receipt was invalid")
        ));
        return;
    }
    const bool projected = model_.updateLibraryState(
        result.state.photo_id,
        result.state.liked,
        result.state.color_label,
        result.state.updated_at_ms
    );
    (void)projected;
    emit colorLabelChanged(result.state.photo_id, result.state.color_label);
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Library organization updated")
    ));
    if (filtered_model_.hasActiveServerFilter()) {
        scheduleFilterQuery();
    }
}

void ReviewController::finishLibraryAlbumsTask() {
    const LibraryAlbumTaskResult result = library_albums_watcher_.result();
    library_albums_task_running_ = false;
    const bool accepted = result.request_id == active_library_albums_request_id_;
    if (accepted && result.has_album_snapshot) {
        library_albums_ = result.albums;
        if (!library_album_id_.isEmpty()) {
            const auto selected = std::find_if(
                library_albums_.cbegin(),
                library_albums_.cend(),
                [this](const BackendLibraryAlbum& album) {
                    return album.id == library_album_id_;
                }
            );
            if (selected == library_albums_.cend()) {
                library_album_id_.clear();
                emit libraryAlbumChanged();
                scheduleFilterQuery();
            }
        }
        emit libraryAlbumsChanged();
    }

    if (accepted && result.error.isEmpty()) {
        if ((result.action == LibraryAlbumTaskAction::AddPhotos
                || result.action == LibraryAlbumTaskAction::RemovePhotos)
            && result.album_id == library_album_id_) {
            scheduleFilterQuery();
        }

        switch (result.action) {
        case LibraryAlbumTaskAction::Refresh:
            break;
        case LibraryAlbumTaskAction::CreateManual:
        case LibraryAlbumTaskAction::CreateSmart:
            setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
                "ReviewController", "Library album created"
            )));
            break;
        case LibraryAlbumTaskAction::Rename:
            setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
                "ReviewController", "Library album renamed"
            )));
            break;
        case LibraryAlbumTaskAction::Delete:
            setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
                "ReviewController", "Library album deleted"
            )));
            break;
        case LibraryAlbumTaskAction::AddPhotos:
            setDecisionStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController", "%1 photos added to the album"
                ),
                {QString::number(result.affected_photo_count)}
            ));
            break;
        case LibraryAlbumTaskAction::RemovePhotos:
            setDecisionStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController", "%1 photos removed from the album"
                ),
                {QString::number(result.affected_photo_count)}
            ));
            break;
        }
    } else if (accepted) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not update Library albums · %1"),
            {result.error}
        ));
    }

    if (library_albums_refresh_pending_ || !accepted) {
        library_albums_refresh_pending_ = false;
        startLibraryAlbumsTask(LibraryAlbumTaskAction::Refresh);
    }
}

void ReviewController::finishLibrarySourceHealthTask() {
    const LibrarySourceHealthTaskResult result = library_source_health_watcher_.result();
    library_source_health_task_running_ = false;
    const bool accepted = result.request_id == active_library_source_health_request_id_;
    if (accepted && result.error.isEmpty()) {
        library_source_health_ = result.sources;
        emit librarySourceHealthChanged();
    } else if (accepted) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not load Library source health · %1"),
            {result.error}
        ));
        emit librarySourceHealthChanged();
    }

    if (library_source_health_refresh_pending_ || !accepted) {
        library_source_health_refresh_pending_ = false;
        startLibrarySourceHealthTask();
    }
}

void ReviewController::finishMissingSourceLocationTask() {
    const MissingSourceLocationTaskResult result = missing_source_locations_watcher_.result();
    missing_source_locations_task_running_ = false;
    const bool accepted = result.request_id == active_missing_source_locations_request_id_
        && result.scan_session_id == missing_source_location_scan_id_;
    if (accepted && result.error.isEmpty()) {
        if (result.page.has_scan) {
            if (result.append) {
                missing_source_locations_ += result.page.items;
            } else {
                missing_source_locations_ = result.page.items;
            }
            missing_source_location_next_cursor_ = result.page.next_location_id;
            missing_source_locations_has_more_ = result.page.has_more;
        } else {
            missing_source_locations_.clear();
            missing_source_location_next_cursor_.clear();
            missing_source_locations_has_more_ = false;
        }
        emit missingSourceLocationReviewChanged();
    } else if (accepted) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not load source scan review · %1"),
            {result.error}
        ));
        emit missingSourceLocationReviewChanged();
    } else {
        // The user switched or closed review while this worker was in flight.
        // Publish the cleared busy state even though its page is intentionally
        // stale and therefore discarded.
        emit missingSourceLocationReviewChanged();
    }

    if (missing_source_locations_refresh_pending_
        && !missing_source_location_scan_id_.isEmpty()) {
        missing_source_locations_refresh_pending_ = false;
        startMissingSourceLocationTask(false);
    }
}

void ReviewController::finishEvidenceTask() {
    ReviewEvidenceTaskResult result = evidence_watcher_.result();
    if (!result.error.isEmpty()) {
        evidence_session_.fail();
        emit comparisonStateChanged();
    setComparisonStatusMessage(review_message(result.kind == ReviewEvidenceTaskKind::Record
                 ? QT_TRANSLATE_NOOP("ReviewController",
                                "Evidence write failed · pair retained · %1")
                 : QT_TRANSLATE_NOOP("ReviewController",
                                "Forget write failed · evidence retained · %1"),
        {result.error})
        );
        return;
    }

    if (result.kind == ReviewEvidenceTaskKind::Record) {
        if (!evidence_session_.completeRecord(result.feedback.event_id)) {
            emit comparisonStateChanged();
      setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController", "Evidence receipt was invalid; pair retained")));
            return;
        }
        emit comparisonStateChanged();
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Preference evidence recorded · sequence %1 · model not active"),
        {result.feedback.sequence})
        );
        emit comparisonRecorded();
        return;
    }

    if (!evidence_session_.completeForget(result.forget.target_event_id)) {
        emit comparisonStateChanged();
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Forget receipt was invalid; evidence retained")));
        return;
    }
    emit comparisonStateChanged();
  setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController",
      "Latest evidence forgotten non-destructively · source event retained"))
    );
    emit comparisonForgotten();
}

void ReviewController::finishDecisionTask() {
    const ReviewDecisionTaskResult result = decision_watcher_.result();
    if (!result.error.isEmpty()) {
        decision_session_.fail();
        if (result.has_authoritative) {
            applyDecisionState(result.authoritative);
        }
        emit decisionStateChanged();
        if (result.is_undo && result.has_authoritative
            && !decision_session_.canUndo()) {
      setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController", "Undo blocked · authoritative decision changed "
                              "outside this session"))
            );
        } else if (result.is_undo && decision_session_.canUndo()) {
      setDecisionStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Undo write failed · unchanged state remains retryable · %1"),
          {result.error})
            );
        } else if (result.has_authoritative) {
      setDecisionStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Decision write failed · authoritative state refreshed · %1"),
          {result.error})
            );
        } else {
      setDecisionStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Decision write failed · refresh also failed · %1 · %2"),
          {result.error, result.refresh_error})
            );
        }
        return;
    }

    if (!decision_session_.complete(result.receipt)) {
        emit decisionStateChanged();
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Decision receipt was invalid; local state retained")));
        return;
    }
    applyDecisionState(result.receipt.after);
    emit decisionStateChanged();
    if (result.is_undo) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Inverse decision appended · sequence %1 · history retained"),
        {result.receipt.sequence})
        );
        emit decisionUndone();
    } else {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Manual decision recorded · sequence %1"),
        {result.receipt.sequence})
        );
    }
}

void ReviewController::startPage(const PageTaskKind kind) {
    if (page_running_ || decision_session_.busy()) {
        return;
    }
    const bool reset = kind != PageTaskKind::Append;
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    page_running_ = true;
    page_reset_running_ = reset;
    active_page_request_id_ = ++page_request_id_;
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    if (reset) {
        if (scan_running_) {
            updateScanStatus();
        } else if (!scan_terminal_error_.isEmpty()) {
      setStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController",
          "Refreshing photos retained before import stopped…")));
        } else if (scan_terminal_cancelled_) {
      setStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController",
          "Refreshing photos retained before import was cancelled…")));
        } else {
      setStatusMessage(review_message(
          QT_TRANSLATE_NOOP("ReviewController", "Loading the local Library…")));
        }
    } else {
        updateReadyStatus();
    }
    page_watcher_.setFuture(QtConcurrent::run([
        backend = backend_,
        filter = currentLibraryFilter(),
        cursor = reset ? BackendLibraryPhotoCursor{} : next_cursor_,
        library_generation = library_generation_,
        request_id = active_page_request_id_,
        kind
    ]() {
        return run_page(
            backend,
            filter,
            cursor,
            library_generation,
            request_id,
            kind
        );
    }));
}

BackendLibraryPhotoFilter ReviewController::currentLibraryFilter() const {
    BackendLibraryPhotoFilter filter;
    const QString flag = filtered_model_.flagFilter();
    if (flag == QStringLiteral("unflagged")) {
        filter.flag = BackendLibraryFlagFilter::Unflagged;
    } else if (flag == QStringLiteral("picked")) {
        filter.flag = BackendLibraryFlagFilter::Picked;
    } else if (flag == QStringLiteral("rejected")) {
        filter.flag = BackendLibraryFlagFilter::Rejected;
    }

    const int minimum_rating = filtered_model_.minimumRating();
    if (minimum_rating > 0) {
        filter.has_minimum_rating = true;
        filter.minimum_rating = static_cast<std::uint8_t>(minimum_rating);
    }

    const QString color = filtered_model_.colorFilter();
    if (color != QStringLiteral("all")) {
        filter.color_label = color;
    }

    const QString edit = filtered_model_.editFilter();
    if (edit == QStringLiteral("edited")) {
        filter.has_development_edits = true;
        filter.development_edits = true;
    } else if (edit == QStringLiteral("unedited")) {
        filter.has_development_edits = true;
        filter.development_edits = false;
    }
    filter.capture_month = filtered_model_.captureMonth();
    filter.camera_key = filtered_model_.cameraKey();
    filter.lens_key = filtered_model_.lensKey();
    filter.album_id = library_album_id_;
    return filter;
}

void ReviewController::startCountQuery() {
    if (count_running_) {
        count_query_pending_ = true;
        return;
    }
    count_running_ = true;
    active_count_request_id_ = ++count_request_id_;
    count_watcher_.setFuture(QtConcurrent::run(
        run_count,
        backend_,
        currentLibraryFilter(),
        library_generation_,
        active_count_request_id_
    ));
}

void ReviewController::startLibraryFacetsTask() {
    if (library_facets_task_running_) {
        library_facets_refresh_pending_ = true;
        return;
    }
    library_facets_task_running_ = true;
    active_library_facets_request_id_ = ++library_facets_request_id_;
    emit libraryFacetsChanged();
    library_facets_watcher_.setFuture(QtConcurrent::run(
        run_library_facets,
        backend_,
        currentLibraryFilter(),
        library_generation_,
        active_library_facets_request_id_
    ));
}

void ReviewController::startLibraryStateMutation(
    const QString& photo_id,
    const bool liked,
    const QString& color_label
) {
    if (photo_id.isEmpty() || library_state_mutation_running_) {
        return;
    }
    library_state_mutation_running_ = true;
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Updating Library organization…"
    )));
    library_state_watcher_.setFuture(QtConcurrent::run(
        run_library_state_mutation,
        backend_,
        photo_id,
        liked,
        color_label
    ));
}

void ReviewController::startLibraryAlbumsTask(
    const LibraryAlbumTaskAction action,
    const QString& name,
    const QString& album_id,
    const QStringList& photo_ids
) {
    if (library_albums_task_running_) {
        library_albums_refresh_pending_ = true;
        return;
    }
    library_albums_task_running_ = true;
    active_library_albums_request_id_ = ++library_albums_request_id_;
    BackendLibraryPhotoFilter smart_query;
    if (action == LibraryAlbumTaskAction::CreateSmart) {
        smart_query = currentLibraryFilter();
        // Smart albums are a stable query, never a nested membership lookup.
        smart_query.album_id.clear();
    }
    emit libraryAlbumsChanged();
    library_albums_watcher_.setFuture(QtConcurrent::run(
        run_library_albums_task,
        backend_,
        action,
        name,
        album_id,
        photo_ids,
        smart_query,
        active_library_albums_request_id_
    ));
}

void ReviewController::startLibrarySourceHealthTask() {
    if (library_source_health_task_running_) {
        library_source_health_refresh_pending_ = true;
        return;
    }
    library_source_health_task_running_ = true;
    active_library_source_health_request_id_ = ++library_source_health_request_id_;
    emit librarySourceHealthChanged();
    library_source_health_watcher_.setFuture(QtConcurrent::run(
        run_library_source_health_task,
        backend_,
        active_library_source_health_request_id_
    ));
}

void ReviewController::startMissingSourceLocationTask(const bool append) {
    if (missing_source_locations_task_running_) {
        missing_source_locations_refresh_pending_ = true;
        return;
    }
    if (missing_source_location_scan_id_.isEmpty()) {
        return;
    }
    missing_source_locations_task_running_ = true;
    active_missing_source_locations_request_id_ = ++missing_source_locations_request_id_;
    emit missingSourceLocationReviewChanged();
    missing_source_locations_watcher_.setFuture(QtConcurrent::run(
        run_missing_source_location_task,
        backend_,
        missing_source_location_scan_id_,
        append ? missing_source_location_next_cursor_ : QString{},
        active_missing_source_locations_request_id_,
        append
    ));
}

void ReviewController::emitWorkStateChanges(
    const bool old_busy,
    const bool old_loading_more,
    const bool old_refreshing
) {
    if (old_busy != busy()) {
        emit busyChanged();
    }
    if (old_loading_more != loadingMore()) {
        emit loadingMoreChanged();
    }
    if (old_refreshing != refreshing()) {
        emit refreshingChanged();
    }
}

void ReviewController::setHasMore(const bool has_more) {
    if (has_more_ == has_more) {
        return;
    }
    has_more_ = has_more;
    emit hasMoreChanged();
}

bool ReviewController::eventFilter(QObject *const watched,
                                   QEvent *const event) {
  if (watched == QCoreApplication::instance() &&
      event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  return QObject::eventFilter(watched, event);
}

void ReviewController::retranslateUi() {
  emit statusTextChanged();
  emit comparisonStatusTextChanged();
  emit decisionStatusTextChanged();
}

void ReviewController::setStatusMessage(LocalizedUiMessage status) {
    if (status_message_ == status) {
        return;
    }
  status_message_ = std::move(status);
    emit statusTextChanged();
}

void ReviewController::updateReadyStatus() {
    if (!scan_terminal_error_.isEmpty()) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Import stopped · %1 photos remain available · filesystem/import "
            "error: %2 · %3 decode failures · %4 preview failures"),
        {
            total_items_,
            scan_terminal_error_,
            decode_hard_failures_,
            preview_failures_,
        })
        );
        return;
    }
    if (scan_terminal_cancelled_) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Import cancelled · %1 photos remain available · %2 "
                          "filesystem issues · %3 decode failures · %4 preview "
                          "failures · %5 decode jobs cancelled"),
        {
            total_items_,
            issue_count_,
            decode_hard_failures_,
            preview_failures_,
            decode_cancelled_,
        })
        );
        return;
    }
    if (total_items_ == 0) {
    setStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Local Library is empty · add a photo folder to begin")));
        return;
    }
    const char *const source = loadingMore() ? QT_TRANSLATE_NOOP(
                "ReviewController",
                "%1 / %2 loaded · %3 supported · %4/%5 preview checks "
                "completed · %6 filesystem issues · %7 decode failures · %8 "
                "preview failures · loading more") : QT_TRANSLATE_NOOP("ReviewController",
                              "%1 / %2 loaded · %3 supported · %4/%5 preview "
                              "checks completed · %6 filesystem issues · %7 "
                              "decode failures · %8 preview failures");
  setStatusMessage(review_message(source, {
                                              model_.rowCount(),
                                              total_items_,
                                              supported_files_,
                                              decode_completed_,
                                              decode_queued_,
                                              issue_count_,
                                              decode_hard_failures_,
                                              preview_failures_,
                                          })
    );
}

void ReviewController::updateScanStatus() {
    const quint64 catalogued = inserted_files_ + unchanged_files_ + revalidation_files_;
    switch (scan_phase_) {
    case BackendScanPhase::PreparingPreviews:
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Import catalogued %1 files · finishing %2 queued "
                          "preview checks · %3 filesystem issues"),
        {catalogued, decode_queued_, issue_count_})
        );
        break;
    case BackendScanPhase::Cancelling:
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Stopping import safely · %1 files retained · queued checks are "
            "being cancelled · current preview may finish"),
        {catalogued})
        );
        break;
    case BackendScanPhase::Discovering:
    default:
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Importing · %1 files checked · %2 supported · %3 catalogued · %4 "
            "preview checks queued · %5 filesystem issues"),
        {
            files_seen_,
            supported_files_,
            catalogued,
            decode_queued_,
            issue_count_,
        })
        );
        break;
    }
}

void ReviewController::setComparisonStatusMessage(LocalizedUiMessage status) {
    if (comparison_status_message_ == status) {
        return;
    }
  comparison_status_message_ = std::move(status);
    emit comparisonStatusTextChanged();
}

void ReviewController::setDecisionStatusMessage(LocalizedUiMessage status) {
    if (decision_status_message_ == status) {
        return;
    }
  decision_status_message_ = std::move(status);
    emit decisionStatusTextChanged();
}

void ReviewController::applyDecisionState(
    const BackendReviewDecisionState& state
) {
    decision_session_.reconcile(state);
    const QString flag = decision_flag_name(state.flag);
    const int rating = static_cast<int>(state.rating);
    const bool projected = model_.updateDecision(
        state.photo_id,
        state.head_sequence,
        flag,
        rating
    );
    (void)projected;
    emit decisionChanged(state.photo_id, state.head_sequence, flag, rating);
    if (filtered_model_.hasActiveServerFilter()) {
        scheduleFilterQuery();
    }
}

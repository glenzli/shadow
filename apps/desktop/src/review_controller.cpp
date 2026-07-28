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

[[nodiscard]] QVector<ReviewItem> review_items(QVector<BackendReviewItem> source) {
    QVector<ReviewItem> items;
    items.reserve(source.size());
    for (auto& item : source) {
        items.push_back({
            .photo_id = std::move(item.photo_id),
            .representation_id = std::move(item.representation_id),
            .visual_handle = std::move(item.visual_handle),
            .decision_head_sequence = item.decision_head_sequence,
            .decision_flag = review_decision_flag_name(item.decision_flag),
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

[[nodiscard]] ReviewComparisonCoordinator::Operations comparison_operations(
    const std::shared_ptr<DesktopBackend>& backend
) {
    if (!backend) {
        throw std::invalid_argument("Review comparison backend is required");
    }
    return {
        .prepare =
            [backend](
                const QString& left_visual_handle,
                const QString& right_visual_handle
            ) {
                return backend->prepareReviewComparison(
                    left_visual_handle,
                    right_visual_handle
                );
            },
        .confirm_ready =
            [backend](
                const QString& presentation_id,
                const QString& left_request_ticket,
                const QString& right_request_ticket
            ) {
                backend->confirmReviewComparisonReady(
                    presentation_id,
                    left_request_ticket,
                    right_request_ticket
                );
            },
        .cancel =
            [backend](const QString& presentation_id) {
                backend->cancelReviewComparison(presentation_id);
            },
        .record =
            [backend](
                const QString& presentation_id,
                const BackendPairwiseOutcome outcome
            ) {
                return backend->recordReviewComparison(
                    presentation_id,
                    outcome
                );
            },
        .forget =
            [backend](const QString& event_id) {
                return backend->forgetReviewFeedback(event_id);
            },
    };
}

[[nodiscard]] ReviewSourceHealthCoordinator::Operations
source_health_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review source-health backend is required");
    }
    return {
        .source_health =
            [backend]() {
                return backend->librarySourceHealth();
            },
        .missing_locations =
            [backend](
                const QString& scan_session_id,
                const QString& after_location_id,
                const std::uint32_t limit
            ) {
                return backend->missingSourceLocationPage(
                    scan_session_id,
                    after_location_id,
                    limit
                );
            },
        .relink =
            [backend](
                const QString& scan_session_id,
                const QString& location_id,
                const QString& candidate_path
            ) {
                return backend->relinkMissingSourceLocation(
                    scan_session_id,
                    location_id,
                    candidate_path
                );
            },
    };
}

[[nodiscard]] ReviewDecisionCoordinator::Operations decision_operations(
    const std::shared_ptr<DesktopBackend>& backend
) {
    if (!backend) {
        throw std::invalid_argument("Review decision backend is required");
    }
    return {
        .mutate =
            [backend](
                const QString& photo_id,
                const std::uint64_t expected_head_sequence,
                const BackendReviewDecisionFlag desired_flag,
                const std::uint8_t desired_rating
            ) {
                return backend->setReviewPhotoDecision(
                    photo_id,
                    expected_head_sequence,
                    desired_flag,
                    desired_rating
                );
            },
        .authoritative_state =
            [backend](const QString& photo_id) {
                return backend->reviewPhotoDecisionState(photo_id);
            },
    };
}

[[nodiscard]] BackendReviewDecisionState backend_decision_state(
    const QString& photo_id,
    const ReviewDecisionValue& value
) {
    const auto flag = review_decision_flag_from_name(value.flag);
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

} // namespace

ReviewController::ReviewController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      photo_inspection_coordinator_(backend_),
      source_health_coordinator_(source_health_operations(backend_)),
      model_(this),
      filtered_model_(this),
      comparison_coordinator_(
          comparison_operations(backend_),
          [this](const QString& ticket) {
              return model_.visualSourceFor(ticket);
          }
      ),
      decision_coordinator_(
          decision_operations(backend_),
          [this](const QString& photo_id)
              -> std::optional<BackendReviewDecisionState> {
              const auto current = model_.decisionFor(photo_id);
              if (!current) {
                  return std::nullopt;
              }
              return backend_decision_state(photo_id, *current);
          },
          [this](const BackendReviewDecisionState& state) {
              projectDecisionState(state);
          }
      ) {
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
        &photo_inspection_coordinator_,
        &ReviewPhotoInspectionCoordinator::stateChanged,
        this,
        &ReviewController::photoInspectionChanged
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::stateChanged,
        this,
        &ReviewController::comparisonStateChanged
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::statusTextChanged,
        this,
        &ReviewController::comparisonStatusTextChanged
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::recorded,
        this,
        &ReviewController::comparisonRecorded
    );
    connect(
        &comparison_coordinator_,
        &ReviewComparisonCoordinator::forgotten,
        this,
        &ReviewController::comparisonForgotten
    );
    connect(
        &decision_coordinator_,
        &ReviewDecisionCoordinator::stateChanged,
        this,
        &ReviewController::decisionStateChanged
    );
    connect(
        &decision_coordinator_,
        &ReviewDecisionCoordinator::statusTextChanged,
        this,
        [this]() {
            setDecisionStatusMessage(decision_coordinator_.statusMessage());
        }
    );
    connect(
        &decision_coordinator_,
        &ReviewDecisionCoordinator::undone,
        this,
        &ReviewController::decisionUndone
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
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::sourceHealthChanged,
        this,
        &ReviewController::librarySourceHealthChanged
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::missingLocationReviewChanged,
        this,
        &ReviewController::missingSourceLocationReviewChanged
    );
    connect(
        &source_health_coordinator_,
        &ReviewSourceHealthCoordinator::globalStatusMessageChanged,
        this,
        [this]() {
            setStatusMessage(
                source_health_coordinator_.globalStatusMessage()
            );
        }
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

QVariantMap ReviewController::photoInspection() const {
    return photo_inspection_coordinator_.presentation();
}

bool ReviewController::photoInspectionBusy() const noexcept {
    return photo_inspection_coordinator_.busy();
}

bool ReviewController::photoInspectionFailed() const noexcept {
    return photo_inspection_coordinator_.failed();
}

bool ReviewController::comparisonBusy() const noexcept {
    return comparison_coordinator_.busy();
}

bool ReviewController::canUndoComparison() const noexcept {
    return comparison_coordinator_.canForget();
}

int ReviewController::sessionEvidenceCount() const noexcept {
    return comparison_coordinator_.activeCount();
}

QString ReviewController::comparisonStatusText() const {
    return comparison_coordinator_.statusText();
}

bool ReviewController::decisionBusy() const noexcept {
    return decision_coordinator_.busy();
}

bool ReviewController::canUndoDecision() const {
    return decision_coordinator_.canUndo();
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

QString ReviewController::filterLiked() const {
    return filtered_model_.likedFilter();
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
    return source_health_coordinator_.sourceHealth();
}

bool ReviewController::librarySourceHealthBusy() const noexcept {
    return source_health_coordinator_.sourceHealthBusy();
}

QVariantList ReviewController::missingSourceLocations() const {
    return source_health_coordinator_.missingLocations();
}

QString ReviewController::missingSourceLocationScanId() const {
    return source_health_coordinator_.missingLocationScanId();
}

bool ReviewController::missingSourceLocationsBusy() const noexcept {
    return source_health_coordinator_.missingLocationsBusy();
}

bool ReviewController::missingSourceLocationsHasMore() const noexcept {
    return source_health_coordinator_.missingLocationsHasMore();
}

bool ReviewController::sourceRelinkBusy() const noexcept {
    return source_health_coordinator_.relinkBusy();
}

QString ReviewController::sourceRelinkStatusText() const {
    return source_health_coordinator_.relinkStatusText();
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
        || comparison_coordinator_.busy() || decision_coordinator_.busy()) {
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

void ReviewController::requestPhotoInspection(
    const QString& photo_id,
    const QString& representation_id
) {
    photo_inspection_coordinator_.request(photo_id, representation_id);
}

void ReviewController::retryPhotoInspection() {
    photo_inspection_coordinator_.retry();
}

void ReviewController::clearPhotoInspection() {
    photo_inspection_coordinator_.clear();
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
        || comparison_coordinator_.busy() || decision_coordinator_.busy()) {
        return;
    }
    startPage(PageTaskKind::Append);
}

QVariantList ReviewController::selectionRangeTargets(
    const QString& anchor_photo_id,
    const QString& anchor_representation_id,
    const QString& photo_id,
    const QString& representation_id
) const {
    if (anchor_photo_id.isEmpty() || anchor_representation_id.isEmpty()
        || photo_id.isEmpty() || representation_id.isEmpty()) {
        return {};
    }

    int anchor_row = -1;
    int target_row = -1;
    const int count = filtered_model_.rowCount();
    for (int row = 0; row < count && (anchor_row < 0 || target_row < 0); ++row) {
        const QModelIndex index = filtered_model_.index(row, 0);
        const QString current_photo_id =
            filtered_model_.data(index, ReviewModel::PhotoIdRole).toString();
        const QString current_representation_id =
            filtered_model_.data(index, ReviewModel::RepresentationIdRole).toString();
        if (current_photo_id == anchor_photo_id
            && current_representation_id == anchor_representation_id) {
            anchor_row = row;
        }
        if (current_photo_id == photo_id
            && current_representation_id == representation_id) {
            target_row = row;
        }
    }
    if (anchor_row < 0 || target_row < 0) {
        return {};
    }

    const int first = std::min(anchor_row, target_row);
    const int last = std::max(anchor_row, target_row);
    QVariantList targets;
    targets.reserve(last - first + 1);
    for (int row = first; row <= last; ++row) {
        const QModelIndex index = filtered_model_.index(row, 0);
        targets.push_back(QVariantMap{
            {QStringLiteral("photoId"),
             filtered_model_.data(index, ReviewModel::PhotoIdRole).toString()},
            {QStringLiteral("representationId"),
             filtered_model_.data(index, ReviewModel::RepresentationIdRole).toString()},
            {QStringLiteral("sourcePath"),
             filtered_model_.data(index, ReviewModel::SourcePathRole).toString()},
            {QStringLiteral("title"),
             filtered_model_.data(index, ReviewModel::TitleRole).toString()},
        });
    }
    return targets;
}

QVariantMap ReviewController::prepareComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) {
    if (comparison_coordinator_.busy() || decision_coordinator_.busy()
        || scan_running_ || refreshing() || page_running_) {
        return {};
    }
    return comparison_coordinator_.prepare(
        left_visual_handle,
        right_visual_handle
    );
}

bool ReviewController::confirmComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) {
    if (decision_coordinator_.busy()) {
        return false;
    }
    return comparison_coordinator_.confirmReady(
        presentation_id,
        left_request_ticket,
        right_request_ticket
    );
}

void ReviewController::cancelComparison(const QString& presentation_id) {
    if (decision_coordinator_.busy()) {
        return;
    }
    comparison_coordinator_.cancel(presentation_id);
}

void ReviewController::recordComparison(
    const QString& presentation_id,
    const int outcome
) {
    const bool admitted = !decision_coordinator_.busy() && !scan_running_
        && !refreshing() && !page_running_;
    static_cast<void>(comparison_coordinator_.record(
        presentation_id,
        outcome,
        admitted
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_coordinator_.busy() || scan_running_ || refreshing()
        || page_running_) {
        return;
    }
    static_cast<void>(comparison_coordinator_.forgetLast());
}

void ReviewController::setPhotoFlag(
    const QString& photo_id,
    const QString& flag
) {
    const bool admitted = !scan_running_ && !refreshing() && !page_running_
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setFlag(photo_id, flag, admitted)
    );
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    const bool admitted = !scan_running_ && !refreshing() && !page_running_
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setRating(photo_id, rating, admitted)
    );
}

void ReviewController::setPhotoColorLabel(
    const QString& photo_id,
    const QString& color_label
) {
    if (library_state_mutation_running_ || comparison_coordinator_.busy()
        || decision_coordinator_.busy()) {
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
    if (library_state_mutation_running_ || comparison_coordinator_.busy()
        || decision_coordinator_.busy()) {
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
    if (scan_running_ || decision_coordinator_.busy()) {
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
    source_health_coordinator_.refreshSourceHealth();
}

void ReviewController::openMissingSourceLocationReview(const QString& scan_session_id) {
    source_health_coordinator_.openMissingLocationReview(scan_session_id);
}

void ReviewController::closeMissingSourceLocationReview() {
    source_health_coordinator_.closeMissingLocationReview();
}

void ReviewController::loadMoreMissingSourceLocations() {
    source_health_coordinator_.loadMoreMissingLocations();
}

void ReviewController::relinkMissingSourceLocation(
    const QString& location_id,
    const QUrl& candidate_url
) {
    source_health_coordinator_.relinkMissingLocation(
        location_id,
        candidate_url
    );
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

void ReviewController::setFilterLiked(const QString& liked) {
    filtered_model_.setLikedFilter(liked);
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
    const bool admitted = !scan_running_ && !refreshing() && !page_running_
        && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.undo(admitted));
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
    if (page_running_ || decision_coordinator_.busy()) {
        return;
    }
    beginFilteredLibraryQuery();
}

void ReviewController::scheduleFilterQuery() {
    library_reset_pending_ = true;
    filter_debounce_timer_.start();
}

void ReviewController::beginFilteredLibraryQuery() {
    if (page_running_ || decision_coordinator_.busy()) {
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
        decision_coordinator_.reconcile(std::move(state));
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
    emit likedChanged(result.state.photo_id, result.state.liked);
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

void ReviewController::startPage(const PageTaskKind kind) {
    if (page_running_ || decision_coordinator_.busy()) {
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

    const QString liked = filtered_model_.likedFilter();
    if (liked == QStringLiteral("liked")) {
        filter.has_liked = true;
        filter.liked = true;
    } else if (liked == QStringLiteral("unliked")) {
        filter.has_liked = true;
        filter.liked = false;
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
  comparison_coordinator_.retranslateUi();
  source_health_coordinator_.retranslateUi();
  decision_coordinator_.retranslateUi();
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

void ReviewController::setDecisionStatusMessage(LocalizedUiMessage status) {
    if (decision_status_message_ == status) {
        return;
    }
  decision_status_message_ = std::move(status);
    emit decisionStatusTextChanged();
}

void ReviewController::projectDecisionState(
    const BackendReviewDecisionState& state
) {
    const QString flag = review_decision_flag_name(state.flag);
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

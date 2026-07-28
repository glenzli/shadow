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
constexpr int FILTER_QUERY_DEBOUNCE_MS = 120;

[[nodiscard]] LocalizedUiMessage review_message(
    const char *const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"ReviewController", source, arguments};
}

[[nodiscard]] ReviewImportCoordinator::Operations import_operations(
    const std::shared_ptr<DesktopBackend>& backend
) {
    if (!backend) {
        throw std::invalid_argument("Review import backend is required");
    }
    return {
        .begin =
            [backend](const quint64 scan_id) {
                backend->beginFolderScan(scan_id);
            },
        .scan =
            [backend](
                const QString& folder_path,
                const quint64 scan_id
            ) {
                return backend->scanFolder(folder_path, scan_id);
            },
        .progress =
            [backend](const quint64 scan_id) {
                return backend->scanProgress(scan_id);
            },
        .cancel =
            [backend](const quint64 scan_id) {
                return backend->cancelFolderScan(scan_id);
            },
    };
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

[[nodiscard]] ReviewLibraryAlbumCoordinator::Operations
album_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library album backend is required");
    }
    return {
        .albums = [backend]() { return backend->libraryAlbums(); },
        .create_manual =
            [backend](const QString& name) {
                static_cast<void>(backend->createManualLibraryAlbum(name));
            },
        .create_smart =
            [backend](
                const QString& name,
                const BackendLibraryPhotoFilter& query
            ) {
                static_cast<void>(backend->createSmartLibraryAlbum(name, query));
            },
        .rename =
            [backend](const QString& album_id, const QString& name) {
                static_cast<void>(backend->renameLibraryAlbum(album_id, name));
            },
        .remove =
            [backend](const QString& album_id) {
                static_cast<void>(backend->deleteLibraryAlbum(album_id));
            },
        .add_photo =
            [backend](const QString& album_id, const QString& photo_id) {
                backend->addPhotoToManualLibraryAlbum(album_id, photo_id);
            },
        .remove_photo =
            [backend](const QString& album_id, const QString& photo_id) {
                static_cast<void>(
                    backend->removePhotoFromManualLibraryAlbum(
                        album_id,
                        photo_id
                    )
                );
            },
    };
}

[[nodiscard]] ReviewLibraryFacetCoordinator::Operations
facet_operations(const std::shared_ptr<DesktopBackend>& backend) {
    if (!backend) {
        throw std::invalid_argument("Review Library facet backend is required");
    }
    return {
        .page =
            [backend](
                const BackendLibraryPhotoFilter& filter,
                const BackendLibraryFacetKind kind,
                const BackendLibraryFacetCursor& cursor,
                const std::uint32_t limit
            ) {
                return backend->libraryFacetPage(
                    filter,
                    kind,
                    cursor,
                    limit
                );
            },
    };
}

[[nodiscard]] ReviewLibraryOrganizationCoordinator::Operations
organization_operations(
    const std::shared_ptr<DesktopBackend>& backend,
    ReviewModel& model
) {
    if (!backend) {
        throw std::invalid_argument(
            "Review Library organization backend is required"
        );
    }
    return {
        .current =
            [&model](const QString& photo_id)
                -> std::optional<
                    ReviewLibraryOrganizationCoordinator::CurrentState
                > {
                const auto current = model.libraryStateFor(photo_id);
                if (!current) {
                    return std::nullopt;
                }
                return ReviewLibraryOrganizationCoordinator::CurrentState{
                    .liked = current->liked,
                    .color_label = current->color_label,
                };
            },
        .mutate =
            [backend](
                const QString& photo_id,
                const bool liked,
                const QString& color_label
            ) {
                return backend->setPhotoLibraryState(
                    photo_id,
                    liked,
                    color_label
                );
            },
        .project =
            [&model](const BackendPhotoLibraryState& state) {
                return model.updateLibraryState(
                    state.photo_id,
                    state.liked,
                    state.color_label,
                    state.updated_at_ms
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
      album_coordinator_(album_operations(backend_)),
      facet_coordinator_(facet_operations(backend_)),
      import_coordinator_(import_operations(backend_)),
      model_(this),
      filtered_model_(this),
      organization_coordinator_(
          organization_operations(backend_, model_)
      ),
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
    filter_debounce_timer_.setInterval(FILTER_QUERY_DEBOUNCE_MS);
    filter_debounce_timer_.setSingleShot(true);
    filter_debounce_timer_.setTimerType(Qt::CoarseTimer);
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
        &facet_coordinator_,
        &ReviewLibraryFacetCoordinator::facetsChanged,
        this,
        &ReviewController::libraryFacetsChanged
    );
    connect(
        &facet_coordinator_,
        &ReviewLibraryFacetCoordinator::globalStatusMessageChanged,
        this,
        [this]() {
            setStatusMessage(facet_coordinator_.globalStatusMessage());
        }
    );
    connect(
        &organization_coordinator_,
        &ReviewLibraryOrganizationCoordinator::stateProjected,
        this,
        [this](
            const QString& photo_id,
            const bool liked,
            const QString& color_label
        ) {
            emit colorLabelChanged(photo_id, color_label);
            emit likedChanged(photo_id, liked);
            if (filtered_model_.hasActiveServerFilter()) {
                scheduleFilterQuery();
            }
        }
    );
    connect(
        &organization_coordinator_,
        &ReviewLibraryOrganizationCoordinator::statusMessageChanged,
        this,
        [this]() {
            setDecisionStatusMessage(
                organization_coordinator_.statusMessage()
            );
        }
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::albumsChanged,
        this,
        &ReviewController::libraryAlbumsChanged
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::albumSelectionChanged,
        this,
        &ReviewController::libraryAlbumChanged
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::queryChanged,
        this,
        &ReviewController::scheduleFilterQuery
    );
    connect(
        &album_coordinator_,
        &ReviewLibraryAlbumCoordinator::statusMessageChanged,
        this,
        [this]() {
            const auto channel = album_coordinator_.statusChannel();
            if (channel
                == ReviewLibraryAlbumCoordinator::StatusChannel::Decision) {
                setDecisionStatusMessage(album_coordinator_.statusMessage());
            } else if (
                channel
                == ReviewLibraryAlbumCoordinator::StatusChannel::Global
            ) {
                setStatusMessage(album_coordinator_.statusMessage());
            }
        }
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
        &import_coordinator_,
        &ReviewImportCoordinator::runningChanged,
        this,
        [this]() {
            emit scanningChanged();
            emit busyChanged();
            emit refreshingChanged();
            emit loadingMoreChanged();
        }
    );
    connect(
        &import_coordinator_,
        &ReviewImportCoordinator::folderPathChanged,
        this,
        &ReviewController::folderPathChanged
    );
    connect(
        &import_coordinator_,
        &ReviewImportCoordinator::progressChanged,
        this,
        [this]() {
            emit scanProgressChanged();
            if (import_coordinator_.takeStreamRefreshRequest(
                    model_.rowCount(),
                    page_running_
                )) {
                startPage(PageTaskKind::StreamingPrefix);
            }
        }
    );
    connect(
        &import_coordinator_,
        &ReviewImportCoordinator::statusMessageChanged,
        this,
        [this]() {
            setStatusMessage(import_coordinator_.statusMessage());
        }
    );
    connect(
        &import_coordinator_,
        &ReviewImportCoordinator::terminalRefreshRequested,
        this,
        [this]() {
            refreshLibrarySourceHealth();
            requestLibraryReset();
        }
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
    filter_debounce_timer_.stop();
    page_watcher_.waitForFinished();
    count_watcher_.waitForFinished();
}

bool ReviewController::busy() const noexcept {
    return model_.rowCount() == 0
        && (scanning() || page_running_ || terminal_refresh_active_);
}

bool ReviewController::scanning() const noexcept {
    return import_coordinator_.scanning();
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
    return import_coordinator_.folderPath();
}

QString ReviewController::statusText() const {
    return status_message_.translated();
}

QVariantMap ReviewController::scanProgress() const {
    return import_coordinator_.progress();
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
    return facet_coordinator_.captureMonths();
}

QVariantList ReviewController::libraryCameraFacets() const {
    return facet_coordinator_.cameras();
}

QVariantList ReviewController::libraryLensFacets() const {
    return facet_coordinator_.lenses();
}

bool ReviewController::libraryFacetsBusy() const noexcept {
    return facet_coordinator_.busy();
}

QString ReviewController::libraryAlbumId() const {
    return album_coordinator_.albumId();
}

QVariantList ReviewController::libraryAlbums() const {
    return album_coordinator_.albums();
}

bool ReviewController::libraryAlbumsBusy() const noexcept {
    return album_coordinator_.busy();
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
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    const bool admitted = !scanning() && !page_running_
        && !terminal_refresh_active_ && !comparison_coordinator_.busy()
        && !decision_coordinator_.busy();
    if (!import_coordinator_.start(folder_url, admitted)) {
        if (!import_coordinator_.statusMessage().isEmpty()) {
            setStatusMessage(import_coordinator_.statusMessage());
        }
        return;
    }
    library_reset_pending_ = false;
    terminal_refresh_active_ = false;
    setHasMore(false);
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
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
    static_cast<void>(import_coordinator_.cancel());
}

void ReviewController::loadMore() {
    if (!has_more_ || scanning() || refreshing() || page_running_
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
        || scanning() || refreshing() || page_running_) {
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
    const bool admitted = !decision_coordinator_.busy() && !scanning()
        && !refreshing() && !page_running_;
    static_cast<void>(comparison_coordinator_.record(
        presentation_id,
        outcome,
        admitted
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_coordinator_.busy() || scanning() || refreshing()
        || page_running_) {
        return;
    }
    static_cast<void>(comparison_coordinator_.forgetLast());
}

void ReviewController::setPhotoFlag(
    const QString& photo_id,
    const QString& flag
) {
    const bool admitted = !scanning() && !refreshing() && !page_running_
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setFlag(photo_id, flag, admitted)
    );
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    const bool admitted = !scanning() && !refreshing() && !page_running_
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setRating(photo_id, rating, admitted)
    );
}

void ReviewController::setPhotoColorLabel(
    const QString& photo_id,
    const QString& color_label
) {
    organization_coordinator_.setColorLabel(
        photo_id,
        color_label,
        !comparison_coordinator_.busy() && !decision_coordinator_.busy()
    );
}

void ReviewController::setPhotoLiked(const QString& photo_id, const bool liked) {
    organization_coordinator_.setLiked(
        photo_id,
        liked,
        !comparison_coordinator_.busy() && !decision_coordinator_.busy()
    );
}

void ReviewController::clearFilters() {
    filtered_model_.clearFilters();
    album_coordinator_.clearAlbumSelection();
}

void ReviewController::refreshVisibleLibrary() {
    if (scanning() || decision_coordinator_.busy()) {
        return;
    }
    requestLibraryReset();
}

void ReviewController::refreshLibraryFacets() {
    if (!scanning()) {
        facet_coordinator_.refresh(
            currentLibraryFilter(),
            library_generation_
        );
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
    album_coordinator_.refresh();
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
    album_coordinator_.createManual(name);
}

void ReviewController::createSmartLibraryAlbum(const QString& name) {
    album_coordinator_.createSmart(name, currentLibraryFilter());
}

void ReviewController::renameLibraryAlbum(
    const QString& album_id,
    const QString& name
) {
    album_coordinator_.rename(album_id, name);
}

void ReviewController::deleteLibraryAlbum(const QString& album_id) {
    album_coordinator_.remove(album_id);
}

void ReviewController::addPhotosToManualLibraryAlbum(
    const QString& album_id,
    const QVariantList& targets
) {
    album_coordinator_.addPhotos(album_id, targets);
}

void ReviewController::removePhotosFromManualLibraryAlbum(
    const QString& album_id,
    const QVariantList& targets
) {
    album_coordinator_.removePhotos(album_id, targets);
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
    album_coordinator_.setAlbumId(album_id);
}

void ReviewController::undoLastDecision() {
    const bool admitted = !scanning() && !refreshing() && !page_running_
        && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.undo(admitted));
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
    if (!scanning()) {
        terminal_refresh_active_ = true;
    }
    facet_coordinator_.refresh(currentLibraryFilter(), library_generation_);
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
        if (result.kind == PageTaskKind::InitialReset && !scanning()) {
            terminal_refresh_active_ = false;
        }
        if (scanning()) {
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
    if (result.kind == PageTaskKind::InitialReset && !scanning()) {
        terminal_refresh_active_ = false;
    }
    emit decisionStateChanged();
    if (library_reset_pending_ && !filter_debounce_timer_.isActive()) {
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        beginFilteredLibraryQuery();
        return;
    }
    if (scanning()) {
        setStatusMessage(import_coordinator_.statusMessage());
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
    if (!scanning() && !page_running_) {
        updateReadyStatus();
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
        if (scanning()) {
            setStatusMessage(import_coordinator_.statusMessage());
        } else {
            setStatusMessage(import_coordinator_.refreshingStatusMessage());
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
    filter.album_id = album_coordinator_.albumId();
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
  album_coordinator_.retranslateUi();
  facet_coordinator_.retranslateUi();
  organization_coordinator_.retranslateUi();
  import_coordinator_.retranslateUi();
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
    setStatusMessage(import_coordinator_.readyStatusMessage(
        total_items_,
        model_.rowCount(),
        loadingMore()
    ));
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

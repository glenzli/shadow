#include "review_controller.hpp"

#include "default_library_reverse_geocoder.hpp"
#include "map_provider_preferences.hpp"
#include "review_controller_backend_operations.hpp"

#include <QCoreApplication>
#include <QEvent>

#include <optional>
#include <utility>

namespace BackendOperations = ReviewControllerBackendOperations;

// Stable QML-facing composition boundary. Request routing lives in the
// responsibility-named review_controller_*.cpp implementation modules.
ReviewController::ReviewController(
    std::shared_ptr<DesktopBackend> backend,
    MapProviderPreferences* const map_provider_preferences,
    const QString& isolated_settings_file,
    std::unique_ptr<SecretStore> remote_library_secret_store,
    QObject* parent
) :
    QObject(parent), backend_(std::move(backend)),
    map_provider_preferences_(map_provider_preferences), photo_inspection_coordinator_(backend_),
    focus_detail_store_(std::make_shared<ReviewFocusDetailStore>()),
    focus_detail_coordinator_(backend_, focus_detail_store_),
    source_health_coordinator_(BackendOperations::source_health_operations(backend_)),
    album_coordinator_(BackendOperations::album_operations(backend_)),
    facet_coordinator_(BackendOperations::facet_operations(backend_)),
    travel_collection_coordinator_(BackendOperations::travel_collection_operations(backend_)),
    place_resolution_coordinator_(
        BackendOperations::place_resolution_operations(backend_),
        makeDefaultLibraryReverseGeocoder(map_provider_preferences_)
    ),
    keyword_coordinator_(BackendOperations::keyword_operations(backend_)),
    map_coordinator_(BackendOperations::map_operations(backend_)),
    metadata_coordinator_(BackendOperations::metadata_operations(backend_)),
    import_coordinator_(BackendOperations::import_operations(backend_)), model_(this),
    source_availability_monitor_(model_, this),
    remote_library_coordinator_(
        BackendOperations::remote_library_operations(backend_),
        model_,
        isolated_settings_file,
        remote_library_secret_store ? std::move(remote_library_secret_store)
                                    : makeVolatileSecretStore(),
        this
    ),
    filtered_model_(this),
    query_coordinator_(BackendOperations::query_operations(backend_), model_),
    organization_coordinator_(BackendOperations::organization_operations(backend_, model_)),
    shared_grade_coordinator_(BackendOperations::shared_grade_operations(backend_)),
    comparison_coordinator_(
        BackendOperations::comparison_operations(backend_),
        [this](const QString& ticket) { return model_.visualSourceFor(ticket); }
    ),
    decision_coordinator_(
        BackendOperations::decision_operations(backend_),
        [this](const QString& photo_id) -> std::optional<BackendReviewDecisionState> {
            const auto current = model_.decisionFor(photo_id);
            if (!current) {
                return std::nullopt;
            }
            return BackendOperations::backend_decision_state(photo_id, *current);
        },
        [this](const BackendReviewDecisionState& state) { projectDecisionState(state); }
    ) {
    // Ordinary Library state is Catalog-backed. The isolated settings file is
    // consumed only by device-local service preferences such as the remote
    // server address; its access token remains in the injected local store.
    Q_ASSERT(map_provider_preferences_ != nullptr);
    initializeCoordinatorWiring();
}

ReviewController::~ReviewController() = default;

bool ReviewController::busy() const noexcept {
    return query_coordinator_.busy();
}

bool ReviewController::scanning() const noexcept {
    return import_coordinator_.scanning();
}

bool ReviewController::refreshing() const noexcept {
    return query_coordinator_.refreshing();
}

bool ReviewController::loadingMore() const noexcept {
    return query_coordinator_.loadingMore();
}

bool ReviewController::hasMore() const noexcept {
    return query_coordinator_.hasMore();
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
    return query_coordinator_.itemCount()
           + (remoteLibraryPresentationEligible() ? remote_library_coordinator_.remotePhotoCount()
                                                  : 0);
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

QString ReviewController::focusDetailImageSource() const {
    return focus_detail_coordinator_.imageSource();
}

QString ReviewController::focusDetailStatusText() const {
    return focus_detail_coordinator_.statusText();
}

bool ReviewController::focusDetailBusy() const noexcept {
    return focus_detail_coordinator_.busy();
}

bool ReviewController::focusDetailReady() const noexcept {
    return focus_detail_coordinator_.ready();
}

bool ReviewController::focusDetailFailed() const noexcept {
    return focus_detail_coordinator_.failed();
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

QString ReviewController::librarySortKey() const {
    return library_sort_key_;
}

bool ReviewController::librarySortDescending() const noexcept {
    return library_sort_descending_;
}

QString ReviewController::filterExcludedFlag() const {
    return filtered_model_.excludedFlagFilter();
}

QString ReviewController::filterExcludedColorLabel() const {
    return filtered_model_.excludedColorFilter();
}

QString ReviewController::filterCaptureMonth() const {
    return filtered_model_.captureMonth();
}

int ReviewController::filterChineseLunarMonth() const noexcept {
    return filtered_model_.chineseLunarMonth();
}

int ReviewController::filterChineseLunarDay() const noexcept {
    return filtered_model_.chineseLunarDay();
}

QString ReviewController::filterChineseLunarMonthType() const {
    return filtered_model_.chineseLunarMonthType();
}

QString ReviewController::filterCameraKey() const {
    return filtered_model_.cameraKey();
}

QString ReviewController::filterLensKey() const {
    return filtered_model_.lensKey();
}

QString ReviewController::filterCountryKey() const {
    return filtered_model_.countryKey();
}

QString ReviewController::filterLocalityKey() const {
    return filtered_model_.localityKey();
}

bool ReviewController::travelFilterEnabled() const noexcept {
    return filtered_model_.travelFilterEnabled();
}

bool ReviewController::dailyFilterEnabled() const noexcept {
    return filtered_model_.dailyFilterEnabled();
}

QStringList ReviewController::filterKeywordIdsAll() const {
    return filtered_model_.keywordIdsAll();
}

QStringList ReviewController::filterExcludedKeywordIdsAny() const {
    return filtered_model_.excludedKeywordIdsAny();
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

QVariantList ReviewController::libraryCountryFacets() const {
    return facet_coordinator_.countries();
}

QVariantList ReviewController::libraryCityFacets() const {
    return facet_coordinator_.cities();
}

bool ReviewController::libraryFacetsBusy() const noexcept {
    return facet_coordinator_.busy();
}

QVariantList ReviewController::livingPlaceCandidates() const {
    return travel_collection_coordinator_.placeCandidates();
}

QVariantList ReviewController::travelGroups() const {
    return travel_collection_coordinator_.groups();
}

qulonglong ReviewController::travelPhotoCount() const noexcept {
    return travel_collection_coordinator_.photoCount();
}

qulonglong ReviewController::dailyPhotoCount() const noexcept {
    return travel_collection_coordinator_.dailyPhotoCount();
}

bool ReviewController::travelCollectionsBusy() const noexcept {
    return travel_collection_coordinator_.busy();
}

QString ReviewController::travelCollectionsErrorText() const {
    return travel_collection_coordinator_.errorText();
}

bool ReviewController::libraryPlaceResolutionRunning() const noexcept {
    return place_resolution_coordinator_.running();
}

QString ReviewController::libraryPlaceResolutionStatusCode() const {
    return place_resolution_coordinator_.statusCode();
}

QString ReviewController::libraryPlaceResolutionErrorText() const {
    return place_resolution_coordinator_.errorText();
}

qulonglong ReviewController::libraryPlaceResolutionProcessedCount() const noexcept {
    return static_cast<qulonglong>(place_resolution_coordinator_.processedCount());
}

qulonglong ReviewController::libraryPlaceResolutionRecordedCount() const noexcept {
    return static_cast<qulonglong>(place_resolution_coordinator_.recordedCount());
}

qulonglong ReviewController::libraryPlaceResolutionFailedCount() const noexcept {
    return static_cast<qulonglong>(place_resolution_coordinator_.failedCount());
}

void ReviewController::retryLibraryPlaceResolution() {
    place_resolution_coordinator_.start();
}

QVariantList ReviewController::libraryKeywords() const {
    return keyword_coordinator_.keywords();
}

QVariantList ReviewController::libraryPhotoKeywords() const {
    return keyword_coordinator_.photoKeywords();
}

bool ReviewController::libraryKeywordsBusy() const noexcept {
    return keyword_coordinator_.busy();
}

QVariantMap ReviewController::librarySystemCollectionCounts() const {
    return facet_coordinator_.systemCollectionCounts();
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

bool ReviewController::librarySourceRemovalBusy() const noexcept {
    return source_health_coordinator_.removeSourceBusy();
}

bool ReviewController::librarySourceReconcileBusy() const noexcept {
    return source_health_coordinator_.reconcileBusy();
}

QVariantMap ReviewController::libraryMetadata() const {
    return metadata_coordinator_.metadata();
}

QVariantMap ReviewController::libraryCaptureTimePreview() const {
    return metadata_coordinator_.captureTimePreview();
}

QVariantMap ReviewController::libraryCoordinateBatchPreview() const {
    return metadata_coordinator_.coordinateBatchPreview();
}

QVariantMap ReviewController::libraryGpxPreview() const {
    return metadata_coordinator_.gpxPreview();
}

QVariantMap ReviewController::libraryMetadataBatchReceipt() const {
    return metadata_coordinator_.batchReceipt();
}

bool ReviewController::libraryMetadataBusy() const noexcept {
    return metadata_coordinator_.busy();
}

QString ReviewController::libraryMetadataStatusCode() const {
    return metadata_coordinator_.statusCode();
}

QString ReviewController::libraryMetadataErrorText() const {
    return metadata_coordinator_.errorText();
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
    return query_coordinator_.itemCount() + visibleRemotePhotoCount();
}

QVariantList ReviewController::sharedGradeNodes() const {
    return shared_grade_coordinator_.nodes();
}

QAbstractItemModel* ReviewController::model() noexcept {
    return &filtered_model_;
}

ReviewModel* ReviewController::reviewModel() noexcept {
    return &model_;
}

std::shared_ptr<ReviewFocusDetailStore> ReviewController::focusDetailStore() const noexcept {
    return focus_detail_store_;
}

bool ReviewController::eventFilter(QObject* const watched, QEvent* const event) {
    if (watched == QCoreApplication::instance()) {
        if (event->type() == QEvent::LanguageChange) {
            retranslateUi();
        } else if (event->type() == QEvent::ApplicationActivate && !scanning()) {
            // Returning to Shadow is the natural low-cost heartbeat boundary:
            // refresh loaded rows immediately and compare each configured
            // folder's lightweight inventory off the UI thread.
            source_availability_monitor_.refreshNow();
            refreshLibrarySourceHealth();
        }
    }
    return QObject::eventFilter(watched, event);
}

void ReviewController::retranslateUi() {
    emit statusTextChanged();
    comparison_coordinator_.retranslateUi();
    source_health_coordinator_.retranslateUi();
    focus_detail_coordinator_.retranslateUi();
    keyword_coordinator_.retranslateUi();
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
        query_coordinator_.totalItems(),
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

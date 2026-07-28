#include "review_controller.hpp"

#include "review_controller_backend_operations.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <algorithm>
#include <optional>
#include <utility>

namespace BackendOperations = ReviewControllerBackendOperations;

ReviewController::ReviewController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      photo_inspection_coordinator_(backend_),
      source_health_coordinator_(
          BackendOperations::source_health_operations(backend_)
      ),
      album_coordinator_(BackendOperations::album_operations(backend_)),
      facet_coordinator_(BackendOperations::facet_operations(backend_)),
      import_coordinator_(BackendOperations::import_operations(backend_)),
      model_(this),
      filtered_model_(this),
      query_coordinator_(
          BackendOperations::query_operations(backend_),
          model_
      ),
      organization_coordinator_(
          BackendOperations::organization_operations(backend_, model_)
      ),
      shared_grade_coordinator_(
          BackendOperations::shared_grade_operations(backend_)
      ),
      comparison_coordinator_(
          BackendOperations::comparison_operations(backend_),
          [this](const QString& ticket) {
              return model_.visualSourceFor(ticket);
          }
      ),
      decision_coordinator_(
          BackendOperations::decision_operations(backend_),
          [this](const QString& photo_id)
              -> std::optional<BackendReviewDecisionState> {
              const auto current = model_.decisionFor(photo_id);
              if (!current) {
                  return std::nullopt;
              }
              return BackendOperations::backend_decision_state(
                  photo_id,
                  *current
              );
          },
          [this](const BackendReviewDecisionState& state) {
              projectDecisionState(state);
          }
      ) {
    // Keep the constructor shape for existing test/application call sites.
    // Library state is Catalog-backed now, so the former desktop-local
    // settings file is deliberately not consulted.
    (void)isolated_settings_file;
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
    return query_coordinator_.itemCount();
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
    return query_coordinator_.itemCount();
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

void ReviewController::scanFolder(const QUrl& folder_url) {
    const bool admitted = !scanning() && !query_coordinator_.pageRunning()
        && !query_coordinator_.refreshing()
        && !comparison_coordinator_.busy()
        && !decision_coordinator_.busy();
    if (!import_coordinator_.start(folder_url, admitted)) {
        if (!import_coordinator_.statusMessage().isEmpty()) {
            setStatusMessage(import_coordinator_.statusMessage());
        }
        return;
    }
    query_coordinator_.clearForImportStart();
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
    const bool admitted = !scanning() && !comparison_coordinator_.busy()
        && !decision_coordinator_.busy();
    static_cast<void>(query_coordinator_.loadMore(admitted));
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
        || scanning() || refreshing() || query_coordinator_.pageRunning()) {
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
        && !refreshing() && !query_coordinator_.pageRunning();
    static_cast<void>(comparison_coordinator_.record(
        presentation_id,
        outcome,
        admitted
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_coordinator_.busy() || scanning() || refreshing()
        || query_coordinator_.pageRunning()) {
        return;
    }
    static_cast<void>(comparison_coordinator_.forgetLast());
}

void ReviewController::setPhotoFlag(
    const QString& photo_id,
    const QString& flag
) {
    const bool admitted = !scanning() && !refreshing()
        && !query_coordinator_.pageRunning()
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setFlag(photo_id, flag, admitted)
    );
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    const bool admitted = !scanning() && !refreshing()
        && !query_coordinator_.pageRunning()
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
            query_coordinator_.generation()
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
    shared_grade_coordinator_.refresh();
}

QVariantMap ReviewController::applySharedGradeNode(
    const QString& layer_id,
    const QVariantList& targets
) {
    return shared_grade_coordinator_.apply(layer_id, targets);
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
    const bool admitted = !scanning() && !refreshing()
        && !query_coordinator_.pageRunning()
        && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.undo(admitted));
}

void ReviewController::requestLibraryReset() {
    query_coordinator_.requestReset(currentLibraryFilter());
}

void ReviewController::scheduleFilterQuery() {
    query_coordinator_.scheduleReset(currentLibraryFilter());
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

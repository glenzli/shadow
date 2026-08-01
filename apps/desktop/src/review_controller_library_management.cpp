#include "review_controller.hpp"

// Library facet, album, source-health, and shared-grade command routing.

void ReviewController::refreshLibraryFacets() {
    if (!scanning()) {
        facet_coordinator_.refresh(currentLibraryFilter(), query_coordinator_.generation());
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
    } else if (normalized_kind == QStringLiteral("country")) {
        setFilterCountryKey(key);
    } else if (normalized_kind == QStringLiteral("city")) {
        setFilterLocalityKey(key);
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

void ReviewController::removeLibrarySource(const QString& source_id, const QString& source_path) {
    if (scanning()) {
        return;
    }
    source_health_coordinator_.removeSource(source_id, source_path);
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
    source_health_coordinator_.relinkMissingLocation(location_id, candidate_url);
}

void ReviewController::relinkUnavailableSourceLocation(
    const QString& location_id,
    const QUrl& candidate_url
) {
    source_health_coordinator_.relinkUnavailableLocation(location_id, candidate_url);
}

void ReviewController::removeUnavailablePhotoFromLibrary(
    const QString& photo_id,
    const QString& title
) {
    source_health_coordinator_.archiveUnavailablePhoto(photo_id, title);
}

void ReviewController::createManualLibraryAlbum(const QString& name) {
    album_coordinator_.createManual(name);
}

void ReviewController::createSmartLibraryAlbum(const QString& name) {
    album_coordinator_.createSmart(name, currentLibraryFilter());
}

void ReviewController::renameLibraryAlbum(const QString& album_id, const QString& name) {
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

QVariantMap
ReviewController::applySharedGradeNode(const QString& layer_id, const QVariantList& targets) {
    return shared_grade_coordinator_.apply(layer_id, targets);
}

#include "review_controller.hpp"

#include <algorithm>

void ReviewController::requestLibraryMetadata(const QString& photo_id) {
    metadata_coordinator_.request(photo_id);
}

void ReviewController::clearLibraryMetadata() {
    metadata_coordinator_.clear();
}

void ReviewController::setLibraryCaptureTime(
    const QString& photo_id,
    const QString& mode,
    const qlonglong captured_at_unix_seconds
) {
    metadata_coordinator_.setCaptureTime(
        photo_id,
        mode.trimmed().toLower(),
        static_cast<std::int64_t>(captured_at_unix_seconds)
    );
}

void ReviewController::setLibraryCoordinates(
    const QString& photo_id,
    const QString& mode,
    const double latitude_degrees,
    const double longitude_degrees,
    const QString& place_name
) {
    metadata_coordinator_.setCoordinates(
        photo_id,
        mode.trimmed().toLower(),
        latitude_degrees,
        longitude_degrees,
        place_name
    );
}

void ReviewController::previewLibraryCaptureTimeBatch(
    const QVariantList& targets,
    const QString& mode,
    const qlonglong offset_seconds
) {
    metadata_coordinator_.previewCaptureTime(
        targets,
        mode.trimmed().toLower(),
        static_cast<std::int64_t>(offset_seconds)
    );
}

void ReviewController::applyLibraryCaptureTimeBatch(
    const QString& preview_id
) {
    metadata_coordinator_.applyCaptureTime(preview_id);
}

void ReviewController::previewLibraryCoordinateBatch(
    const QVariantList& targets,
    const QString& mode,
    const double latitude_degrees,
    const double longitude_degrees,
    const QString& place_name,
    const QString& source_label
) {
    metadata_coordinator_.previewCoordinates(
        targets,
        mode.trimmed().toLower(),
        latitude_degrees,
        longitude_degrees,
        place_name.trimmed(),
        source_label.trimmed()
    );
}

void ReviewController::applyLibraryCoordinateBatch(
    const QString& preview_id
) {
    metadata_coordinator_.applyCoordinates(preview_id);
}

void ReviewController::previewLibraryGpxImport(
    const QUrl& gpx_url,
    const QVariantList& targets,
    const qlonglong camera_clock_offset_seconds,
    const int maximum_gap_seconds
) {
    if (!gpx_url.isLocalFile()) {
        return;
    }
    metadata_coordinator_.previewGpx(
        gpx_url.toLocalFile(),
        targets,
        static_cast<std::int64_t>(camera_clock_offset_seconds),
        static_cast<std::uint32_t>(std::clamp(maximum_gap_seconds, 0, 86'400))
    );
}

void ReviewController::applyLibraryGpxImport(const QString& preview_id) {
    metadata_coordinator_.applyGpx(preview_id);
}

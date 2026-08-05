#include "review_controller.hpp"

// QML-facing routing for the selected-photo metadata lifecycle.

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

void ReviewController::requestFocusDetail(
    const QString& photo_id,
    const QString& source_path,
    const double center_x,
    const double center_y
) {
    focus_detail_coordinator_.request(photo_id, source_path, center_x, center_y);
}

void ReviewController::clearFocusDetail() {
    focus_detail_coordinator_.clear();
}

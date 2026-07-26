#include "edit_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr int CROP_PREVIEW_THROTTLE_MS = 16;

} // namespace

QVariantMap EditController::photoGeometry() const {
    const auto& geometry = grade_stack_.geometry;
    return {
        {QStringLiteral("cropLeft"), geometry.crop_left},
        {QStringLiteral("cropTop"), geometry.crop_top},
        {QStringLiteral("cropRight"), geometry.crop_right},
        {QStringLiteral("cropBottom"), geometry.crop_bottom},
        {QStringLiteral("quarterTurn"), static_cast<int>(geometry.quarter_turn)},
        {QStringLiteral("straightenDegrees"), geometry.straighten_degrees},
        {QStringLiteral("flipHorizontal"), geometry.flip_horizontal},
        {QStringLiteral("flipVertical"), geometry.flip_vertical},
        {QStringLiteral("identity"), geometry == BackendPhotoGeometry{}},
    };
}

bool EditController::cropToolActive() const noexcept {
    return crop_tool_active_;
}

void EditController::rotatePhotoClockwise() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.quarter_turn = static_cast<std::uint8_t>(
        (static_cast<unsigned int>(grade_stack_.geometry.quarter_turn) + 1U) % 4U
    );
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/rotate_clockwise"), before);
}

void EditController::rotatePhotoCounterClockwise() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.quarter_turn = static_cast<std::uint8_t>(
        (static_cast<unsigned int>(grade_stack_.geometry.quarter_turn) + 3U) % 4U
    );
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/rotate_counterclockwise"), before);
}

void EditController::flipPhotoHorizontally() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const bool quarter_turn_is_odd = grade_stack_.geometry.quarter_turn % 2U != 0U;
    if (quarter_turn_is_odd) {
        grade_stack_.geometry.flip_vertical = !grade_stack_.geometry.flip_vertical;
    } else {
        grade_stack_.geometry.flip_horizontal = !grade_stack_.geometry.flip_horizontal;
    }
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/flip_horizontal"), before);
}

void EditController::flipPhotoVertically() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const bool quarter_turn_is_odd = grade_stack_.geometry.quarter_turn % 2U != 0U;
    if (quarter_turn_is_odd) {
        grade_stack_.geometry.flip_horizontal = !grade_stack_.geometry.flip_horizontal;
    } else {
        grade_stack_.geometry.flip_vertical = !grade_stack_.geometry.flip_vertical;
    }
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/flip_vertical"), before);
}

void EditController::setCenteredPhotoCropAspectRatio(
    const double output_aspect_ratio,
    const double current_output_aspect_ratio
) {
    if (!active_ || interactionLocked() || !std::isfinite(output_aspect_ratio)
        || !std::isfinite(current_output_aspect_ratio) || output_aspect_ratio <= 0.0
        || current_output_aspect_ratio <= 0.0) {
        return;
    }
    constexpr double minimum_aspect_ratio = 1.0 / 8.0;
    constexpr double maximum_aspect_ratio = 8.0;
    if (output_aspect_ratio < minimum_aspect_ratio
        || output_aspect_ratio > maximum_aspect_ratio) {
        return;
    }

    // The canvas reports post-orientation dimensions. Crop is stored in the
    // original source coordinate system, so odd quarter-turns invert the
    // ratio before the centered crop is solved. That keeps “4:5” visually
    // consistent whether the photo was rotated before or after choosing it.
    const bool quarter_turn_is_odd = grade_stack_.geometry.quarter_turn % 2U != 0U;
    const double source_target_aspect = quarter_turn_is_odd
        ? 1.0 / output_aspect_ratio : output_aspect_ratio;
    const double source_current_aspect = quarter_turn_is_odd
        ? 1.0 / current_output_aspect_ratio : current_output_aspect_ratio;
    if (!std::isfinite(source_target_aspect) || !std::isfinite(source_current_aspect)
        || source_target_aspect <= 0.0 || source_current_aspect <= 0.0) {
        return;
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    auto& geometry = grade_stack_.geometry;
    const double width = geometry.crop_right - geometry.crop_left;
    const double height = geometry.crop_bottom - geometry.crop_top;
    if (source_current_aspect > source_target_aspect) {
        const double reduced_width = width * source_target_aspect / source_current_aspect;
        const double midpoint = (geometry.crop_left + geometry.crop_right) * 0.5;
        geometry.crop_left = midpoint - reduced_width * 0.5;
        geometry.crop_right = midpoint + reduced_width * 0.5;
    } else {
        const double reduced_height = height * source_current_aspect / source_target_aspect;
        const double midpoint = (geometry.crop_top + geometry.crop_bottom) * 0.5;
        geometry.crop_top = midpoint - reduced_height * 0.5;
        geometry.crop_bottom = midpoint + reduced_height * 0.5;
    }
    geometry.crop_left = std::clamp(geometry.crop_left, 0.0, 1.0);
    geometry.crop_top = std::clamp(geometry.crop_top, 0.0, 1.0);
    geometry.crop_right = std::clamp(geometry.crop_right, 0.0, 1.0);
    geometry.crop_bottom = std::clamp(geometry.crop_bottom, 0.0, 1.0);
    if (geometry == before.geometry) {
        return;
    }
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/crop/aspect"), before);
}

void EditController::setPhotoCropBounds(
    const double crop_left,
    const double crop_top,
    const double crop_right,
    const double crop_bottom
) {
    constexpr double minimum_crop_extent = 0.01;
    if (!active_ || interactionLocked()
        || !std::isfinite(crop_left) || !std::isfinite(crop_top)
        || !std::isfinite(crop_right) || !std::isfinite(crop_bottom)) {
        return;
    }

    const double left = std::clamp(crop_left, 0.0, 1.0);
    const double top = std::clamp(crop_top, 0.0, 1.0);
    const double right = std::clamp(crop_right, 0.0, 1.0);
    const double bottom = std::clamp(crop_bottom, 0.0, 1.0);
    if (right - left < minimum_crop_extent
        || bottom - top < minimum_crop_extent) {
        return;
    }

    const BackendGradeStack before = grade_stack_;
    auto& geometry = grade_stack_.geometry;
    geometry.crop_left = left;
    geometry.crop_top = top;
    geometry.crop_right = right;
    geometry.crop_bottom = bottom;
    if (geometry == before.geometry) {
        return;
    }

    setFullResolutionState(false, false, 0);
    recordWorkingTransition(
        gradeNodeHistoryKey(QStringLiteral("geometry/crop/bounds")),
        before
    );
    notifyParametersChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    // While the crop tool is open the preview intentionally renders the
    // complete oriented source. Moving only the overlay must not launch
    // dozens of identical RAW renders; leaving the tool renders the crop.
    if (!crop_tool_active_) {
        schedulePreview(CROP_PREVIEW_THROTTLE_MS);
    }
}

void EditController::setCropToolActive(const bool active) {
    if (crop_tool_active_ == active) {
        return;
    }
    finishActiveGesture();
    crop_tool_active_ = active;
    resetDetailState();
    first_interactive_frame_presented_ = false;
    cancelActivePreview(true);
    emit cropToolActiveChanged();
    if (active_) {
        schedulePreview(0);
    }
}

void EditController::setPhotoStraightenDegrees(const double degrees) {
    if (!active_ || interactionLocked() || !std::isfinite(degrees)
        || degrees < -45.0 || degrees > 45.0
        || grade_stack_.geometry.straighten_degrees == degrees) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.straighten_degrees = degrees;
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/straighten"), before);
}

void EditController::resetPhotoGeometry() {
    if (!active_ || interactionLocked() || grade_stack_.geometry == BackendPhotoGeometry{}) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry = BackendPhotoGeometry{};
    setFullResolutionState(false, false, 0);
    parameterEdited(QStringLiteral("geometry/reset"), before);
}

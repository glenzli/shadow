#pragma once

#include "backend/edit_types.hpp"

#include <cstdint>
#include <optional>
#include <utility>

// Before compares colour at the same framing, not an uncropped image stretched
// into the edited rectangle. This bounded projection deliberately excludes
// grading, RAW white balance/denoise, repairs, completion rasters and Liquify.
[[nodiscard]] inline BackendGradeStack neutral_before_stack(
    const BackendGradeStack& current,
    BackendPhotoGeometry presentation_geometry,
    const bool crop_tool_active
) {
    BackendGradeStack result;
    result.foundation.enabled = current.foundation.enabled;
    result.foundation.optics = current.foundation.optics;
    if (crop_tool_active) {
        presentation_geometry.crop_left = 0;
        presentation_geometry.crop_top = 0;
        presentation_geometry.crop_right = 1;
        presentation_geometry.crop_bottom = 1;
    }
    result.geometry = presentation_geometry;
    return result;
}

// Independent of ordinary edit revisions: colour gestures keep the cached
// baseline, while a new photo or framing invalidates both pixels and analysis.
class EditBeforePreviewState final {
public:
    [[nodiscard]] bool observe(const std::uint64_t photo, BackendGradeStack stack) {
        if (stack_ && photo == photo_ && *stack_ == stack)
            return false;
        photo_ = photo;
        stack_ = std::move(stack);
        ++revision_;
        return true;
    }

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const BackendGradeStack& stack() const { return stack_.value(); }

private:
    std::uint64_t photo_ = 0;
    std::uint64_t revision_ = 0;
    std::optional<BackendGradeStack> stack_;
};

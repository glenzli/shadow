#pragma once

#include "desktop_backend.hpp"

#include <stdexcept>

namespace PointColorModel {

[[nodiscard]] inline int count(const BackendFineEditParameters& fine) noexcept {
    return (fine.color_range_enabled || !fine.additional_point_colors.isEmpty() ? 1 : 0)
        + static_cast<int>(fine.additional_point_colors.size());
}

[[nodiscard]] inline BackendPointColorRange at(
    const BackendFineEditParameters& fine,
    const int index
) {
    if (index == 0 && count(fine) > 0) {
        return {
            .enabled = fine.color_range_enabled,
            .center_degrees = fine.color_range_center,
            .width_degrees = fine.color_range_width,
            .softness = fine.color_range_softness,
            .hue_shift_degrees = fine.color_range_hue,
            .saturation = fine.color_range_saturation,
            .lightness = fine.color_range_lightness,
        };
    }
    if (index > 0 && index - 1 < fine.additional_point_colors.size()) {
        return fine.additional_point_colors.at(index - 1);
    }
    throw std::out_of_range("invalid Point Color index");
}

inline void set(
    BackendFineEditParameters& fine,
    const int index,
    const BackendPointColorRange& range
) {
    if (index == 0) {
        fine.color_range_enabled = range.enabled;
        fine.color_range_center = range.center_degrees;
        fine.color_range_width = range.width_degrees;
        fine.color_range_softness = range.softness;
        fine.color_range_hue = range.hue_shift_degrees;
        fine.color_range_saturation = range.saturation;
        fine.color_range_lightness = range.lightness;
        return;
    }
    if (index > 0 && index - 1 < fine.additional_point_colors.size()) {
        fine.additional_point_colors[index - 1] = range;
        return;
    }
    throw std::out_of_range("invalid Point Color index");
}

} // namespace PointColorModel

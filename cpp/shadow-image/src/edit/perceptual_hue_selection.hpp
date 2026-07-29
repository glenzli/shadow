#pragma once

#include "working_color_math.hpp"

namespace shadow::image::detail {

// One Oklab hue observation with confidence suppressed near the neutral axis.
// Keeping this selection primitive shared prevents Point Color and conditional
// masks from assigning unstable, effectively random hues to nearly gray pixels.
struct PerceptualHueSample final {
    double degrees = 0.0;
    double confidence = 0.0;
};

[[nodiscard]] double normalized_hue_degrees(double degrees) noexcept;

[[nodiscard]] double signed_hue_distance_degrees(double hue, double center) noexcept;

[[nodiscard]] PerceptualHueSample sample_oklab_hue(const Vector3& lab) noexcept;

// width_degrees is the selected half-width. softness moves the fully selected
// inner edge toward the center while retaining width as the zero-coverage edge.
[[nodiscard]] double perceptual_hue_range_weight(
    double hue,
    double center_degrees,
    double width_degrees,
    double softness
) noexcept;

} // namespace shadow::image::detail

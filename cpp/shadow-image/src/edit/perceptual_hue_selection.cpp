#include "perceptual_hue_selection.hpp"

#include <algorithm>
#include <cmath>

namespace shadow::image::detail {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;

[[nodiscard]] double
smooth_transition(const double lower, const double upper, const double value) noexcept {
    if (value <= lower) {
        return 0.0;
    }
    if (value >= upper) {
        return 1.0;
    }
    const double normalized = (value - lower) / (upper - lower);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

} // namespace

double normalized_hue_degrees(const double degrees) noexcept {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

double signed_hue_distance_degrees(const double hue, const double center) noexcept {
    return std::remainder(hue - center, 360.0);
}

PerceptualHueSample sample_oklab_hue(const Vector3& lab) noexcept {
    const double chroma = std::hypot(lab[1], lab[2]);
    const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
    return PerceptualHueSample{
        .degrees = normalized_hue_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi),
        .confidence = smooth_transition(0.002, 0.02, relative_chroma),
    };
}

double perceptual_hue_range_weight(
    const double hue,
    const double center_degrees,
    const double width_degrees,
    const double softness
) noexcept {
    const double distance = std::abs(signed_hue_distance_degrees(hue, center_degrees));
    const double feather = width_degrees * softness;
    if (feather <= 0.0) {
        return distance <= width_degrees ? 1.0 : 0.0;
    }
    const double fully_selected = width_degrees - feather;
    return 1.0 - smooth_transition(fully_selected, width_degrees, distance);
}

} // namespace shadow::image::detail

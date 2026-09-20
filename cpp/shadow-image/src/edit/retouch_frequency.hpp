#pragma once
#include "retouch_source_transform.hpp"
#include <shadow/image/working_rgb.hpp>
#include <span>

namespace shadow::image::detail {
// Replaces only one component of the immutable pre-stroke RGB source. The
// Gaussian is evaluated in the union of target and donor plus its exact halo.
void apply_frequency_retouch(
    FloatRgbImage& image,
    const FloatRgbImage& source,
    std::span<const float> coverage,
    std::int64_t left,
    std::int64_t top,
    std::uint32_t width,
    std::uint32_t height,
    const RetouchSourceMapping& mapping,
    double strength,
    bool texture,
    std::uint16_t frequency_radius
);
} // namespace shadow::image::detail

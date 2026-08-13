#pragma once

#include "retouch_source_transform.hpp"

#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace shadow::image::detail {

// Gradient-domain Heal consumes one compact target coverage mask and a
// same-shaped donor offset. It preserves donor texture, fits a robust local
// illumination plane to the target boundary, and solves the repaired interior
// against that boundary; final feathering remains the authored coverage
// contract.
void apply_texture_heal(
    FloatRgbImage& destination,
    const FloatRgbImage& source,
    std::span<const float> coverage,
    std::int64_t coverage_origin_x,
    std::int64_t coverage_origin_y,
    std::uint32_t coverage_width,
    std::uint32_t coverage_height,
    const RetouchSourceMapping& source_mapping,
    double strength,
    bool preserve_target_structure
);

} // namespace shadow::image::detail

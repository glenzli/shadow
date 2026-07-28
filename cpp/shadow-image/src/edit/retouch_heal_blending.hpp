#pragma once

#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace shadow::image::detail {

// Gradient-domain Heal consumes one compact target coverage mask and a
// same-shaped donor offset. It preserves donor texture while solving the
// repaired interior against the target boundary; final feathering remains the
// authored coverage contract.
void apply_texture_heal(FloatRgbImage& destination, const FloatRgbImage& source,
                        std::span<const float> coverage, std::int64_t coverage_origin_x,
                        std::int64_t coverage_origin_y, std::uint32_t coverage_width,
                        std::uint32_t coverage_height, double source_offset_x_pixels,
                        double source_offset_y_pixels);

} // namespace shadow::image::detail

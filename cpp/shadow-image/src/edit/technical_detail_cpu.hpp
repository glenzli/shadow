#pragma once

#include <cstddef>

namespace shadow::image {

struct AdjustmentFootprint;
struct AdjustmentNode;
struct FloatRgbImage;
struct SharpenAdjustment;

namespace detail {

[[nodiscard]] AdjustmentFootprint technical_detail_footprint(const SharpenAdjustment& parameters,
                                                             double level_zero_to_raster_scale_x,
                                                             double level_zero_to_raster_scale_y);

// Runs the complete technical recovery order: denoise, dehaze/defringe, then
// capture sharpening. Planning and execution consume one internal support plan.
void apply_technical_detail_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                std::size_t node_index, const SharpenAdjustment& parameters);

} // namespace detail

} // namespace shadow::image

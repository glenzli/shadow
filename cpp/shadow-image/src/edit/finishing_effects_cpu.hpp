#pragma once

#include <cstddef>

namespace shadow::image {

struct AdjustmentExecutionContext;
struct AdjustmentNode;
struct FloatRgbImage;
struct SharpenAdjustment;

namespace detail {

// Applies the coordinate-stable final detail pass. Grain and vignette remain
// one owner because they share full-raster coordinates and a fused RGB write.
void apply_finishing_effects_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                 std::size_t node_index, const SharpenAdjustment& parameters,
                                 const AdjustmentExecutionContext& context);

} // namespace detail

} // namespace shadow::image

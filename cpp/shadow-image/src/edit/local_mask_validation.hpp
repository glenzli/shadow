#pragma once

#include <shadow/image/adjustment_layers.hpp>

namespace shadow::image::detail {

void validate_local_mask(const LocalMask& mask);

// Shared admission for CPU execution and resident GPU layer lowering. It validates every layer
// and enclosed node, including bypassed content, then returns the full-image coordinate space used
// by normalized masks.
[[nodiscard]] Dimensions validate_adjustment_layer_plan(
    const FloatRgbImage& input,
    std::span<const AdjustmentLayer> layers,
    AdjustmentExecutionContext context
);
[[nodiscard]] Dimensions validate_adjustment_layer_plan(
    Dimensions input_dimensions,
    std::span<const AdjustmentLayer> layers,
    AdjustmentExecutionContext context
);

} // namespace shadow::image::detail

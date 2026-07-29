#pragma once

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_brush_index.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"

#include <shadow/image/adjustment_layers.hpp>

#include <optional>
#include <string>

namespace shadow::image::detail {

// One complete lowering of LocalMask into the mirrored Metal ABI. Layer blending and coverage
// capture consume this same prepared contract so five-kind mask math, brush topology, inversion,
// and working-space conversion cannot drift.
struct WarmGpuMaskPlan final {
    WarmLayerBlendParameters parameters;
    std::optional<WarmGpuBrushIndex> brush_index;
};

struct WarmGpuMaskPlanPreparation final {
    std::optional<WarmGpuMaskPlan> plan;
    std::string diagnostic;
};

[[nodiscard]] WarmGpuMaskPlanPreparation prepare_warm_gpu_mask_plan(
    const FloatRgbImage& source_layout,
    const LocalMask& mask,
    Dimensions full_dimensions,
    AdjustmentExecutionContext context,
    double opacity
);

} // namespace shadow::image::detail

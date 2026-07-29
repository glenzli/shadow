#pragma once

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_brush_index.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"

#include <shadow/image/adjustment_layers.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shadow::image::detail {

struct WarmGpuLayerPlanEntry final {
    std::size_t layer_index = 0U;
    EditExecutionPlan execution;
    WarmLayerBlendParameters blend;
    std::optional<WarmGpuBrushIndex> brush_index;
    bool needs_blend = false;
};

struct WarmGpuLayerPlan final {
    std::vector<WarmGpuLayerPlanEntry> active_layers;
    bool complete = true;
    std::string diagnostic;
};

[[nodiscard]] WarmGpuLayerPlan prepare_warm_gpu_layer_plan(
    const FloatRgbImage& source_layout,
    std::span<const AdjustmentLayer> layers,
    WarmEditGpuRenderContext context
);

} // namespace shadow::image::detail

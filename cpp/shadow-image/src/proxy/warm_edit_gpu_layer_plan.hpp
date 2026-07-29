#pragma once

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_mask_plan.hpp"

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
    struct MaskCoverageCapture final {
        std::uint32_t layer_index = 0U;
        WarmGpuMaskPlan mask;
    };
    std::optional<MaskCoverageCapture> mask_coverage;
    bool complete = true;
    std::string diagnostic;
};

[[nodiscard]] WarmGpuLayerPlan prepare_warm_gpu_layer_plan(
    const FloatRgbImage& source_layout,
    std::span<const AdjustmentLayer> layers,
    WarmEditGpuRenderContext context,
    std::optional<std::uint32_t> target_layer_index = std::nullopt
);

} // namespace shadow::image::detail

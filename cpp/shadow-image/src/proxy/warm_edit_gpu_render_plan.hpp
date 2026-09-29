#pragma once

#include "warm_edit_gpu_neighbourhood_plan.hpp"

#include <shadow/image/edit_execution_plan.hpp>

#include <vector>

namespace shadow::image::detail {

struct WarmGpuRenderPass final {
    EditExecutionPlan before;
    WarmGpuNeighbourhoodStage neighbourhood;
};

struct WarmGpuRenderPlan final {
    std::vector<WarmGpuRenderPass> passes;
    EditExecutionPlan after;
    bool complete = true;

    [[nodiscard]] bool has_neighbourhood_stage() const noexcept {
        return !passes.empty();
    }
};

[[nodiscard]] WarmGpuRenderPlan prepare_warm_gpu_render_plan(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y,
    AdjustmentExecutionContext context,
    bool resident_highlight_evidence_available
);

} // namespace shadow::image::detail

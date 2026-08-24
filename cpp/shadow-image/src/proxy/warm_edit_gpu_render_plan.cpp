#include "warm_edit_gpu_render_plan.hpp"

#include <shadow/image/edit_execution_plan.hpp>

#include <utility>

namespace shadow::image::detail {

WarmGpuRenderPlan prepare_warm_gpu_render_plan(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y,
    const AdjustmentExecutionContext context,
    const bool resident_highlight_evidence_available
) {
    WarmGpuRenderPlan result{
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
    };
    EditExecutionPlan pending{
        .source_node_count = plan.source_node_count,
    };
    for (const EditExecutionSegment& segment : plan.segments) {
        if (segment.locality == AdjustmentLocality::pixel_local) {
            pending.segments.push_back(segment);
            continue;
        }
        for (const EditExecutionStep& step : segment.steps) {
            auto stage = prepare_warm_gpu_neighbourhood_stage(
                nodes,
                step,
                dimensions,
                working_space,
                level_zero_to_raster_scale_x,
                level_zero_to_raster_scale_y,
                context,
                resident_highlight_evidence_available
            );
            if (!stage.has_value()) {
                result.complete = false;
                result.passes.clear();
                result.after.segments.clear();
                return result;
            }
            result.passes.push_back(
                WarmGpuRenderPass{
                    .before = std::move(pending),
                    .neighbourhood = std::move(*stage),
                }
            );
            pending = EditExecutionPlan{
                .source_node_count = plan.source_node_count,
            };
        }
    }
    result.after = std::move(pending);
    return result;
}

} // namespace shadow::image::detail

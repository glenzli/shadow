#include "warm_edit_gpu_layer_plan.hpp"

#include "../edit/local_mask_validation.hpp"

#include <shadow/image/edit_execution_plan.hpp>

#include <utility>

namespace shadow::image::detail {

WarmGpuLayerPlan prepare_warm_gpu_layer_plan(
    const FloatRgbImage& source_layout,
    const std::span<const AdjustmentLayer> layers,
    const WarmEditGpuRenderContext context
) {
    const Dimensions full =
        validate_adjustment_layer_plan(source_layout, layers, context.adjustment);
    WarmGpuLayerPlan result;
    result.active_layers.reserve(layers.size());
    for (std::size_t index = 0U; index < layers.size(); ++index) {
        const AdjustmentLayer& layer = layers[index];
        if (!layer.enabled || layer.opacity == 0.0) {
            continue;
        }
        EditExecutionPlan execution = compile_edit_execution_plan(
            layer.nodes,
            source_layout.level_zero_to_raster_scale_x,
            source_layout.level_zero_to_raster_scale_y
        );
        if (execution.segments.empty()) {
            continue;
        }
        WarmLayerBlendParameters blend{
            .width = source_layout.dimensions.width,
            .height = source_layout.dimensions.height,
            .input_row_floats =
                static_cast<std::uint32_t>(source_layout.row_stride_bytes / sizeof(float)),
            .origin_x = context.adjustment.origin_x,
            .origin_y = context.adjustment.origin_y,
            .full_width = full.width,
            .full_height = full.height,
            .opacity = static_cast<float>(layer.opacity),
        };
        if (layer.mask.has_value()) {
            const LocalMask& mask = *layer.mask;
            switch (mask.kind) {
            case LocalMaskKind::linear_gradient:
                blend.mask_kind = WarmLayerMaskKind::linear_gradient;
                break;
            case LocalMaskKind::radial_gradient:
                blend.mask_kind = WarmLayerMaskKind::radial_gradient;
                break;
            case LocalMaskKind::brush:
                result.complete = false;
                result.active_layers.clear();
                result.diagnostic =
                    "resident Metal brush masks require the indexed brush-mask stage";
                return result;
            }
            blend.invert = mask.invert ? 1U : 0U;
            blend.x0 = static_cast<float>(mask.x0);
            blend.y0 = static_cast<float>(mask.y0);
            blend.x1 = static_cast<float>(mask.x1);
            blend.y1 = static_cast<float>(mask.y1);
            blend.radius_x = static_cast<float>(mask.radius_x);
            blend.radius_y = static_cast<float>(mask.radius_y);
            blend.feather = static_cast<float>(mask.feather);
        }
        result.active_layers.push_back(
            WarmGpuLayerPlanEntry{
                .layer_index = index,
                .execution = std::move(execution),
                .blend = blend,
                .needs_blend = layer.mask.has_value() || layer.opacity != 1.0,
            }
        );
    }
    return result;
}

} // namespace shadow::image::detail

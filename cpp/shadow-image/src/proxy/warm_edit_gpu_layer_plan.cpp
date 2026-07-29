#include "warm_edit_gpu_layer_plan.hpp"

#include "../edit/local_mask_validation.hpp"
#include "../edit/working_color_math.hpp"
#include "warm_edit_gpu_color_matrix.hpp"

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
    std::optional<WorkingSpaceTransform> condition_mask_transform;
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
            case LocalMaskKind::luminance_range:
                blend.mask_kind = WarmLayerMaskKind::luminance_range;
                break;
            case LocalMaskKind::color_range:
                blend.mask_kind = WarmLayerMaskKind::color_range;
                break;
            case LocalMaskKind::brush: {
                if (mask.points.empty()) {
                    if (!mask.invert) {
                        continue;
                    }
                    // Inverting an empty brush selects the complete frame.
                    blend.mask_kind = WarmLayerMaskKind::full_frame;
                    blend.invert = 0U;
                    result.active_layers.push_back(
                        WarmGpuLayerPlanEntry{
                            .layer_index = index,
                            .execution = std::move(execution),
                            .blend = blend,
                            .brush_index = std::nullopt,
                            .needs_blend = layer.opacity != 1.0,
                        }
                    );
                    continue;
                }
                auto preparation = prepare_warm_gpu_brush_index(mask, full);
                if (!preparation.index.has_value()) {
                    result.complete = false;
                    result.active_layers.clear();
                    result.diagnostic =
                        preparation.diagnostic.empty()
                            ? "resident Metal could not prepare the brush spatial index"
                            : std::move(preparation.diagnostic);
                    return result;
                }
                blend.mask_kind = WarmLayerMaskKind::brush;
                blend.brush_grid_columns = preparation.index->grid_columns;
                blend.brush_grid_rows = preparation.index->grid_rows;
                blend.brush_capsule_count = preparation.index->capsule_count;
                blend.brush_reference_count = preparation.index->reference_count;
                blend.invert = mask.invert ? 1U : 0U;
                blend.radius_x = static_cast<float>(mask.radius_x);
                blend.feather = static_cast<float>(mask.feather);
                result.active_layers.push_back(
                    WarmGpuLayerPlanEntry{
                        .layer_index = index,
                        .execution = std::move(execution),
                        .blend = blend,
                        .brush_index = std::move(preparation.index),
                        .needs_blend = true,
                    }
                );
                continue;
            }
            }
            if (mask.kind == LocalMaskKind::luminance_range
                || mask.kind == LocalMaskKind::color_range) {
                if (!condition_mask_transform.has_value()) {
                    condition_mask_transform =
                        prepare_working_space_transform(source_layout.working_space);
                }
                if (!fill_warm_color_matrix_rows(
                        condition_mask_transform->rgb_to_xyz,
                        blend.rgb_to_xyz_row_0,
                        blend.rgb_to_xyz_row_1,
                        blend.rgb_to_xyz_row_2
                    )) {
                    result.complete = false;
                    result.active_layers.clear();
                    result.diagnostic =
                        "resident Metal could not encode the condition-mask working space";
                    return result;
                }
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
                .brush_index = std::nullopt,
                .needs_blend = layer.mask.has_value() || layer.opacity != 1.0,
            }
        );
    }
    return result;
}

} // namespace shadow::image::detail

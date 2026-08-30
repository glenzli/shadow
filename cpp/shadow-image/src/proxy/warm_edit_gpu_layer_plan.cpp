#include "warm_edit_gpu_layer_plan.hpp"

#include "../edit/local_mask_validation.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace shadow::image::detail {

WarmGpuLayerPlan prepare_warm_gpu_layer_plan(
    const FloatRgbImage& source_layout,
    const std::span<const AdjustmentLayer> layers,
    const WarmEditGpuRenderContext context,
    const std::optional<std::uint32_t> target_layer_index
) {
    if (target_layer_index.has_value()
        && static_cast<std::size_t>(*target_layer_index) >= layers.size()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "mask coverage target layer index is outside the resident adjustment-layer plan"
        );
    }
    const Dimensions full =
        validate_adjustment_layer_plan(source_layout, layers, context.adjustment);
    WarmGpuLayerPlan result;
    result.active_layers.reserve(layers.size());
    if (std::any_of(layers.begin(), layers.end(), [](const AdjustmentLayer& layer) {
            return layer.mask.has_value() && !layer.mask->components.empty();
        })) {
        result.complete = false;
        result.diagnostic =
            "composite local masks require exact CPU replay; resident Metal supports legacy leaves only";
        return result;
    }
    if (target_layer_index.has_value()) {
        const AdjustmentLayer& target = layers[*target_layer_index];
        if (target.mask.has_value()) {
            auto preparation = prepare_warm_gpu_mask_plan(
                source_layout,
                *target.mask,
                full,
                context.adjustment,
                1.0
            );
            if (!preparation.plan.has_value()) {
                result.complete = false;
                result.diagnostic =
                    preparation.diagnostic.empty()
                        ? "resident Metal could not prepare mask coverage capture"
                        : std::move(preparation.diagnostic);
                return result;
            }
            result.mask_coverage = WarmGpuLayerPlan::MaskCoverageCapture{
                .layer_index = *target_layer_index,
                .mask = std::move(*preparation.plan),
            };
        }
    }
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
        std::optional<WarmGpuBrushIndex> brush_index;
        if (layer.mask.has_value()) {
            auto preparation = prepare_warm_gpu_mask_plan(
                source_layout,
                *layer.mask,
                full,
                context.adjustment,
                layer.opacity
            );
            if (!preparation.plan.has_value()) {
                result.complete = false;
                result.active_layers.clear();
                result.diagnostic =
                    preparation.diagnostic.empty()
                        ? "resident Metal could not prepare the layer mask"
                        : std::move(preparation.diagnostic);
                return result;
            }
            blend = preparation.plan->parameters;
            brush_index = std::move(preparation.plan->brush_index);
            if (blend.mask_kind == WarmLayerMaskKind::empty) {
                continue;
            }
            blend.use_precomputed_coverage =
                result.mask_coverage.has_value()
                    && result.mask_coverage->layer_index == index
                ? 1U
                : 0U;
        }
        result.active_layers.push_back(
            WarmGpuLayerPlanEntry{
                .layer_index = index,
                .execution = std::move(execution),
                .blend = blend,
                .brush_index = std::move(brush_index),
                .needs_blend = layer.mask.has_value() || layer.opacity != 1.0,
            }
        );
    }
    return result;
}

} // namespace shadow::image::detail

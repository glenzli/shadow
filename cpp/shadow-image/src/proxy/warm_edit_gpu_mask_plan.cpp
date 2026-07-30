#include "warm_edit_gpu_mask_plan.hpp"

#include "../edit/working_color_math.hpp"
#include "warm_edit_gpu_color_matrix.hpp"

#include <utility>

namespace shadow::image::detail {

WarmGpuMaskPlanPreparation prepare_warm_gpu_mask_plan(
    const FloatRgbImage& source_layout,
    const LocalMask& mask,
    const Dimensions full_dimensions,
    const AdjustmentExecutionContext context,
    const double opacity
) {
    WarmGpuMaskPlan plan{
        .parameters =
            WarmLayerBlendParameters{
                .width = source_layout.dimensions.width,
                .height = source_layout.dimensions.height,
                .input_row_floats =
                    static_cast<std::uint32_t>(source_layout.row_stride_bytes / sizeof(float)),
                .origin_x = context.origin_x,
                .origin_y = context.origin_y,
                .full_width = full_dimensions.width,
                .full_height = full_dimensions.height,
                .opacity = static_cast<float>(opacity),
            },
    };
    auto& parameters = plan.parameters;
    switch (mask.kind) {
    case LocalMaskKind::linear_gradient:
        parameters.mask_kind = WarmLayerMaskKind::linear_gradient;
        break;
    case LocalMaskKind::radial_gradient:
        parameters.mask_kind = WarmLayerMaskKind::radial_gradient;
        break;
    case LocalMaskKind::luminance_range:
        parameters.mask_kind = WarmLayerMaskKind::luminance_range;
        break;
    case LocalMaskKind::color_range:
        parameters.mask_kind = WarmLayerMaskKind::color_range;
        break;
    case LocalMaskKind::managed_raster:
        return WarmGpuMaskPlanPreparation{
            .plan = std::nullopt,
            .diagnostic =
                "resident Metal does not yet support immutable managed raster masks",
        };
    case LocalMaskKind::brush: {
        if (mask.points.empty()) {
            parameters.mask_kind =
                mask.invert ? WarmLayerMaskKind::full_frame : WarmLayerMaskKind::empty;
            parameters.invert = 0U;
            return WarmGpuMaskPlanPreparation{
                .plan = std::move(plan),
                .diagnostic = {},
            };
        }
        auto preparation = prepare_warm_gpu_brush_index(mask, full_dimensions);
        if (!preparation.index.has_value()) {
            return WarmGpuMaskPlanPreparation{
                .plan = std::nullopt,
                .diagnostic =
                    preparation.diagnostic.empty()
                        ? "resident Metal could not prepare the brush spatial index"
                        : std::move(preparation.diagnostic),
            };
        }
        parameters.mask_kind = WarmLayerMaskKind::brush;
        parameters.brush_grid_columns = preparation.index->grid_columns;
        parameters.brush_grid_rows = preparation.index->grid_rows;
        parameters.brush_capsule_count = preparation.index->capsule_count;
        parameters.brush_reference_count = preparation.index->reference_count;
        plan.brush_index = std::move(preparation.index);
        break;
    }
    }
    if (mask.kind == LocalMaskKind::luminance_range
        || mask.kind == LocalMaskKind::color_range) {
        const WorkingSpaceTransform transform =
            prepare_working_space_transform(source_layout.working_space);
        if (!fill_warm_color_matrix_rows(
                transform.rgb_to_xyz,
                parameters.rgb_to_xyz_row_0,
                parameters.rgb_to_xyz_row_1,
                parameters.rgb_to_xyz_row_2
            )) {
            return WarmGpuMaskPlanPreparation{
                .plan = std::nullopt,
                .diagnostic =
                    "resident Metal could not encode the condition-mask working space",
            };
        }
    }
    parameters.invert = mask.invert ? 1U : 0U;
    parameters.x0 = static_cast<float>(mask.x0);
    parameters.y0 = static_cast<float>(mask.y0);
    parameters.x1 = static_cast<float>(mask.x1);
    parameters.y1 = static_cast<float>(mask.y1);
    parameters.radius_x = static_cast<float>(mask.radius_x);
    parameters.radius_y = static_cast<float>(mask.radius_y);
    parameters.feather = static_cast<float>(mask.feather);
    return WarmGpuMaskPlanPreparation{
        .plan = std::move(plan),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail

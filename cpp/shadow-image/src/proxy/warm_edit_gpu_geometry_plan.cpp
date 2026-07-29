#include "warm_edit_gpu_geometry_plan.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace shadow::image::detail {

namespace {

[[nodiscard]] bool is_transposed(const PhotoQuarterTurn quarter_turn) noexcept {
    return quarter_turn == PhotoQuarterTurn::clockwise_90
           || quarter_turn == PhotoQuarterTurn::clockwise_270;
}

[[nodiscard]] bool
contains(const GeometryPixelRect& outer, const GeometryPixelRect& inner) noexcept {
    const std::uint64_t outer_right = static_cast<std::uint64_t>(outer.x) + outer.width;
    const std::uint64_t outer_bottom = static_cast<std::uint64_t>(outer.y) + outer.height;
    const std::uint64_t inner_right = static_cast<std::uint64_t>(inner.x) + inner.width;
    const std::uint64_t inner_bottom = static_cast<std::uint64_t>(inner.y) + inner.height;
    return inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right
           && inner_bottom <= outer_bottom;
}

} // namespace

bool WarmGpuGeometryPlan::valid() const noexcept {
    const auto& value = parameters;
    const std::uint64_t input_pixels =
        static_cast<std::uint64_t>(value.input_width) * value.input_height;
    const std::uint64_t output_pixels =
        static_cast<std::uint64_t>(value.output_width) * value.output_height;
    return value.input_width > 0U && value.input_height > 0U
           && value.input_row_floats >= value.input_width * 3U && value.source_crop_width > 0U
           && value.source_crop_height > 0U && value.output_canvas_width > 0U
           && value.output_canvas_height > 0U && value.output_width > 0U && value.output_height > 0U
           && value.output_origin_x <= value.output_canvas_width
           && value.output_origin_y <= value.output_canvas_height
           && value.output_width <= value.output_canvas_width - value.output_origin_x
           && value.output_height <= value.output_canvas_height - value.output_origin_y
           && value.quarter_turn <= 3U && value.flip_horizontal <= 1U && value.flip_vertical <= 1U
           && std::isfinite(value.straighten_cosine) && std::isfinite(value.straighten_sine)
           && output_pixels <= input_pixels
           && output_dimensions == Dimensions{value.output_width, value.output_height}
           && std::isfinite(output_level_zero_to_raster_scale_x)
           && output_level_zero_to_raster_scale_x > 0.0
           && std::isfinite(output_level_zero_to_raster_scale_y)
           && output_level_zero_to_raster_scale_y > 0.0;
}

WarmGpuGeometryPreparation prepare_warm_gpu_geometry_plan(
    const Dimensions resident_dimensions,
    const std::uint32_t input_row_floats,
    const double input_level_zero_to_raster_scale_x,
    const double input_level_zero_to_raster_scale_y,
    const WarmEditGpuGeometryContext& context
) {
    const auto failed = [](std::string diagnostic) {
        return WarmGpuGeometryPreparation{
            .plan = std::nullopt,
            .diagnostic = std::move(diagnostic),
        };
    };
    if (resident_dimensions.width == 0U || resident_dimensions.height == 0U
        || resident_dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U
        || input_row_floats < resident_dimensions.width * 3U
        || !std::isfinite(input_level_zero_to_raster_scale_x)
        || input_level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(input_level_zero_to_raster_scale_y)
        || input_level_zero_to_raster_scale_y <= 0.0) {
        return failed("photo geometry received an invalid resident source layout");
    }
    if (context.source_tile_rect.width != resident_dimensions.width
        || context.source_tile_rect.height != resident_dimensions.height
        || context.layout.source_crop.width == 0U || context.layout.source_crop.height == 0U
        || context.layout.output_dimensions.width == 0U
        || context.layout.output_dimensions.height == 0U) {
        return failed("photo geometry does not describe the resident source tile");
    }

    GeometryPixelRect required;
    try {
        required = photo_geometry_source_rect_for_output(
            context.layout,
            context.geometry,
            context.output_rect
        );
    } catch (const std::exception& error) {
        return failed(error.what());
    }
    if (!contains(context.source_tile_rect, required)) {
        return failed("photo geometry source tile does not contain the sampling footprint");
    }

    const double radians =
        context.geometry.straighten_degrees * 3.141592653589793238462643383279502884 / 180.0;
    WarmGpuGeometryPlan plan{
        .parameters =
            WarmPhotoGeometryParameters{
                .input_width = resident_dimensions.width,
                .input_height = resident_dimensions.height,
                .input_row_floats = input_row_floats,
                .source_tile_origin_x = context.source_tile_rect.x,
                .source_tile_origin_y = context.source_tile_rect.y,
                .source_crop_origin_x = context.layout.source_crop.x,
                .source_crop_origin_y = context.layout.source_crop.y,
                .source_crop_width = context.layout.source_crop.width,
                .source_crop_height = context.layout.source_crop.height,
                .output_canvas_width = context.layout.output_dimensions.width,
                .output_canvas_height = context.layout.output_dimensions.height,
                .output_origin_x = context.output_rect.x,
                .output_origin_y = context.output_rect.y,
                .output_width = context.output_rect.width,
                .output_height = context.output_rect.height,
                .quarter_turn = static_cast<std::uint32_t>(context.geometry.quarter_turn),
                .flip_horizontal = context.geometry.flip_horizontal ? 1U : 0U,
                .flip_vertical = context.geometry.flip_vertical ? 1U : 0U,
                .straighten_cosine = static_cast<float>(std::cos(radians)),
                .straighten_sine = static_cast<float>(std::sin(radians)),
            },
        .output_dimensions =
            {
                context.output_rect.width,
                context.output_rect.height,
            },
        .output_level_zero_to_raster_scale_x = is_transposed(context.geometry.quarter_turn)
                                                   ? input_level_zero_to_raster_scale_y
                                                   : input_level_zero_to_raster_scale_x,
        .output_level_zero_to_raster_scale_y = is_transposed(context.geometry.quarter_turn)
                                                   ? input_level_zero_to_raster_scale_x
                                                   : input_level_zero_to_raster_scale_y,
    };
    if (!plan.valid()) {
        return failed("photo geometry exceeds the resident Metal output contract");
    }
    return WarmGpuGeometryPreparation{
        .plan = std::move(plan),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail

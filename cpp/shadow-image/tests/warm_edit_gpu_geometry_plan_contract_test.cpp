#include "../src/proxy/warm_edit_gpu_geometry_plan.hpp"

#include <shadow/image/photo_geometry.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void complete_canvas_and_bounded_tiles_share_one_mapping_contract() {
    constexpr image::Dimensions dimensions{320U, 200U};
    const image::PhotoGeometry geometry{
        .crop_left = 0.1,
        .crop_top = 0.15,
        .crop_right = 0.9,
        .crop_bottom = 0.85,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 7.0,
        .flip_horizontal = true,
    };
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const image::GeometryPixelRect complete_output{
        .width = layout.output_dimensions.width,
        .height = layout.output_dimensions.height,
    };
    const image::GeometryPixelRect complete_source{
        .width = dimensions.width,
        .height = dimensions.height,
    };
    const auto complete = image::detail::prepare_warm_gpu_geometry_plan(
        dimensions,
        dimensions.width * 3U,
        0.5,
        0.25,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = complete_source,
            .output_rect = complete_output,
        }
    );
    expect(
        complete.plan.has_value() && complete.plan->valid()
            && complete.plan->output_dimensions == layout.output_dimensions
            && complete.plan->parameters.output_canvas_width == layout.output_dimensions.width
            && complete.plan->parameters.quarter_turn == 1U
            && complete.plan->output_level_zero_to_raster_scale_x == 0.25
            && complete.plan->output_level_zero_to_raster_scale_y == 0.5,
        "complete transposed geometry lowers the authoritative canvas and native scales"
    );

    const image::GeometryPixelRect output_tile{
        .x = layout.output_dimensions.width / 4U,
        .y = layout.output_dimensions.height / 5U,
        .width = layout.output_dimensions.width / 3U,
        .height = layout.output_dimensions.height / 2U,
    };
    const auto source_tile =
        image::photo_geometry_source_rect_for_output(layout, geometry, output_tile);
    const auto bounded = image::detail::prepare_warm_gpu_geometry_plan(
        {source_tile.width, source_tile.height},
        source_tile.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = source_tile,
            .output_rect = output_tile,
        }
    );
    expect(
        bounded.plan.has_value() && bounded.plan->valid()
            && bounded.plan->parameters.source_tile_origin_x == source_tile.x
            && bounded.plan->parameters.source_tile_origin_y == source_tile.y
            && bounded.plan->parameters.output_origin_x == output_tile.x
            && bounded.plan->parameters.output_origin_y == output_tile.y,
        "bounded detail tiles retain full-canvas output and source-space origins"
    );

    auto incomplete_source = source_tile;
    --incomplete_source.width;
    const auto incomplete = image::detail::prepare_warm_gpu_geometry_plan(
        {incomplete_source.width, incomplete_source.height},
        incomplete_source.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = incomplete_source,
            .output_rect = output_tile,
        }
    );
    expect(
        !incomplete.plan.has_value() && !incomplete.diagnostic.empty(),
        "a source tile missing any bilinear footprint declines before Metal encoding"
    );

    auto displaced_source = source_tile;
    displaced_source.x += displaced_source.width + 1U;
    const auto displaced = image::detail::prepare_warm_gpu_geometry_plan(
        {displaced_source.width, displaced_source.height},
        displaced_source.width * 3U,
        1.0,
        1.0,
        image::detail::WarmEditGpuGeometryContext{
            .layout = layout,
            .geometry = geometry,
            .source_tile_rect = displaced_source,
            .output_rect = output_tile,
        }
    );
    expect(
        !displaced.plan.has_value() && !displaced.diagnostic.empty(),
        "a disjoint source tile cannot pass containment through unsigned subtraction"
    );
}

} // namespace

int main() {
    complete_canvas_and_bounded_tiles_share_one_mapping_contract();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

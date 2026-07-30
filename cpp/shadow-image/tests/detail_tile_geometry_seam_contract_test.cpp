#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void transformed_irregular_tiles_match_the_full_geometry_canvas() {
    constexpr image::Dimensions dimensions{160U, 112U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
    const image::PhotoGeometry geometry{
        .crop_left = 0.07,
        .crop_top = 0.09,
        .crop_right = 0.94,
        .crop_bottom = 0.91,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = -5.75,
        .flip_vertical = true,
    };
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "geometry-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.27},
        },
        image::AdjustmentNode{
            .node_id = "geometry-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.84},
        },
    };
    const image::DetailTileRect full_rect{
        0U,
        0U,
        layout.output_dimensions.width,
        layout.output_dimensions.height,
    };
    const auto full = session.render_rgb8(plan, full_rect, geometry);
    bool all_used_metal = full.execution.backend == image::DetailTileRenderBackend::metal;
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    const std::uint32_t split_x = layout.output_dimensions.width / 3U;
    const std::uint32_t split_y = layout.output_dimensions.height / 2U;
    const std::array tiles{
        image::DetailTileRect{0U, 0U, split_x, split_y},
        image::DetailTileRect{
            split_x,
            0U,
            layout.output_dimensions.width - split_x,
            split_y,
        },
        image::DetailTileRect{0U, split_y, split_x, layout.output_dimensions.height - split_y},
        image::DetailTileRect{
            split_x,
            split_y,
            layout.output_dimensions.width - split_x,
            layout.output_dimensions.height - split_y,
        },
    };
    for (const auto rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect, geometry);
        all_used_metal =
            all_used_metal && tile.execution.backend == image::DetailTileRenderBackend::metal;
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * layout.output_dimensions.width + rect.x)
                * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "crop, rotation, mirror and straighten are invariant across irregular detail tiles"
    );
    expect(
        !image::adjustment_backend_available(image::AdjustmentBackend::metal) || all_used_metal,
        "transformed full-detail tiles stay on resident Metal"
    );
}

void transformed_layer_tiles_match_the_full_geometry_canvas() {
    constexpr image::Dimensions dimensions{144U, 96U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.07,
        .crop_right = 0.92,
        .crop_bottom = 0.94,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_270,
        .straighten_degrees = 4.25,
        .flip_horizontal = true,
    };
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "geometry-layer",
            .opacity = 0.76,
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "layer-exposure",
                    .parameters = image::ExposureAdjustment{.stops = -0.22},
                },
                image::AdjustmentNode{
                    .node_id = "layer-contrast",
                    .parameters = image::ContrastAdjustment{
                        .factor = 1.16,
                        .pivot = 0.18,
                    },
                },
            },
        },
    };
    const image::DetailTileRect full_rect{
        0U,
        0U,
        layout.output_dimensions.width,
        layout.output_dimensions.height,
    };
    const auto full = session.render_rgb8_layers(layers, full_rect, geometry);
    bool all_used_metal = full.execution.backend == image::DetailTileRenderBackend::metal;
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    const std::uint32_t split = layout.output_dimensions.width / 2U;
    const std::array tiles{
        image::DetailTileRect{0U, 0U, split, layout.output_dimensions.height},
        image::DetailTileRect{
            split,
            0U,
            layout.output_dimensions.width - split,
            layout.output_dimensions.height,
        },
    };
    for (const auto rect : tiles) {
        const auto tile = session.render_rgb8_layers(layers, rect, geometry);
        all_used_metal =
            all_used_metal && tile.execution.backend == image::DetailTileRenderBackend::metal;
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(row) * layout.output_dimensions.width + rect.x) * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "transformed layer composition is invariant across full-detail tile boundaries"
    );
    expect(
        !image::adjustment_backend_available(image::AdjustmentBackend::metal) || all_used_metal,
        "transformed full-detail layer tiles stay on resident Metal"
    );
}

void liquify_and_canvas_are_seam_free_across_cpu_detail_tiles() {
    constexpr image::Dimensions dimensions{160U, 112U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
    const image::PhotoGeometry geometry{
        .crop_left = 0.06,
        .crop_top = 0.08,
        .crop_right = 0.95,
        .crop_bottom = 0.92,
        .straighten_degrees = 3.25,
    };
    const image::PhotoLiquify liquify{
        .strokes =
            {
                image::PhotoLiquifyPushStroke{
                    .points =
                        {
                            {.x = 0.35, .y = 0.52, .pressure = 1.0},
                            {.x = 0.58, .y = 0.47, .pressure = 0.8},
                        },
                    .radius = 0.14,
                    .strength = 0.35,
                    .hardness = 0.55,
                },
            },
    };
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "liquify-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.18},
        },
    };
    const auto layout = image::photo_geometry_layout(dimensions, geometry);
    const image::DetailTileRect full_rect{
        0U,
        0U,
        layout.output_dimensions.width,
        layout.output_dimensions.height,
    };
    const auto full = session.render_rgb8(plan, full_rect, geometry, &liquify);
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    const std::uint32_t split_x = layout.output_dimensions.width / 2U;
    const std::uint32_t split_y = layout.output_dimensions.height / 2U;
    const std::array tiles{
        image::DetailTileRect{0U, 0U, split_x, split_y},
        image::DetailTileRect{
            split_x,
            0U,
            layout.output_dimensions.width - split_x,
            split_y,
        },
        image::DetailTileRect{0U, split_y, split_x, layout.output_dimensions.height - split_y},
        image::DetailTileRect{
            split_x,
            split_y,
            layout.output_dimensions.width - split_x,
            layout.output_dimensions.height - split_y,
        },
    };
    for (const auto rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect, geometry, &liquify);
        expect(
            tile.execution.backend == image::DetailTileRenderBackend::cpu,
            "Liquify detail explicitly uses the portable CPU backend"
        );
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * layout.output_dimensions.width + rect.x)
                * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "Liquify plus Canvas remains invariant across conservative detail-tile preimages"
    );
}

} // namespace

int main() {
    transformed_irregular_tiles_match_the_full_geometry_canvas();
    transformed_layer_tiles_match_the_full_geometry_canvas();
    liquify_and_canvas_are_seam_free_across_cpu_detail_tiles();
    return failures == 0 ? 0 : 1;
}

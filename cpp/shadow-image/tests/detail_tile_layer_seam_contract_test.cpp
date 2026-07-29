#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/full_edit_detail.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

[[nodiscard]] std::array<image::AdjustmentLayer, 4U> photographic_layers() {
    return {
        image::AdjustmentLayer{
            .layer_id = "global-opacity",
            .opacity = 0.68,
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "global-exposure",
                        .parameters = image::ExposureAdjustment{.stops = 0.22},
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "linear-sky",
            .opacity = 0.79,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::linear_gradient,
                    .x0 = 0.12,
                    .y0 = 0.08,
                    .x1 = 0.86,
                    .y1 = 0.73,
                    .invert = true,
                },
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "sky-exposure",
                        .parameters = image::ExposureAdjustment{.stops = -0.34},
                    },
                    image::AdjustmentNode{
                        .node_id = "sky-saturation",
                        .parameters = image::SaturationAdjustment{.factor = 0.88},
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "radial-subject-detail",
            .opacity = 0.87,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::radial_gradient,
                    .x0 = 0.58,
                    .y0 = 0.47,
                    .radius_x = 0.29,
                    .radius_y = 0.32,
                    .feather = 0.57,
                },
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "subject-detail",
                        .parameter_schema_version = image::detail_effects_parameter_schema_version,
                        .implementation_version = image::color_grading_implementation_version,
                        .parameters =
                            image::SharpenAdjustment{
                                .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                                .clarity = 0.27,
                                .texture = 0.23,
                                .local_contrast = 0.21,
                                .local_contrast_scale = 0.66,
                            },
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "continuous-brush-dodge",
            .opacity = 0.74,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::brush,
                    .radius_x = 0.055,
                    .feather = 0.46,
                    .points =
                        {
                            {.x = 0.04, .y = 0.18, .begins_stroke = true},
                            {.x = 0.27, .y = 0.31},
                            {.x = 0.51, .y = 0.49},
                            {.x = 0.78, .y = 0.72},
                            {.x = 0.88, .y = 0.20, .begins_stroke = true},
                            {.x = 0.67, .y = 0.34},
                        },
                },
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "brush-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.29},
                },
            },
        },
    };
}

void resident_gradient_layers_preserve_full_resolution_tile_seams() {
    constexpr image::Dimensions dimensions{224U, 96U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto layers = photographic_layers();
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");

    const auto full =
        session.render_rgb8_layers(layers, {0U, 0U, dimensions.width, dimensions.height});
    bool all_tiles_used_metal = full.execution.backend == image::DetailTileRenderBackend::metal;
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array tiles{
        image::DetailTileRect{0U, 0U, 73U, 37U},
        image::DetailTileRect{73U, 0U, 76U, 37U},
        image::DetailTileRect{149U, 0U, 75U, 37U},
        image::DetailTileRect{0U, 37U, 73U, 59U},
        image::DetailTileRect{73U, 37U, 76U, 59U},
        image::DetailTileRect{149U, 37U, 75U, 59U},
    };
    for (const image::DetailTileRect rect : tiles) {
        const auto tile = session.render_rgb8_layers(layers, rect);
        all_tiles_used_metal =
            all_tiles_used_metal && tile.execution.backend == image::DetailTileRenderBackend::metal;
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }

    expect(
        stitched == full.bytes,
        "opacity, gradients, continuous brush and creative detail preserve exact tile seams"
    );
    expect(
        !image::adjustment_backend_available(image::AdjustmentBackend::metal)
            || all_tiles_used_metal,
        "gradient and continuous brush layers stay on resident Metal at full-resolution detail"
    );
}

} // namespace

int main() {
    resident_gradient_layers_preserve_full_resolution_tile_seams();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

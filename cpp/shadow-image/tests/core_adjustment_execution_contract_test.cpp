#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {
void exposure_preserves_unclipped_scene_range_and_padding() {
    const auto input = rgb_image(1, {-0.25F, 0.5F, 1.5F, 37.0F}, 1U);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{.stops = 1.0},
        },
    };
    const auto output = image::execute_adjustment_nodes(input, nodes);
    expect_close(output.samples[0], -0.5F, "exposure keeps negative scene-linear values");
    expect_close(output.samples[1], 1.0F, "one exposure stop doubles middle values");
    expect_close(output.samples[2], 3.0F, "exposure does not clamp values above one");
    expect_close(output.samples[3], 37.0F, "row padding is not processed as a pixel");
    expect_close(input.samples[1], 0.5F, "node execution does not mutate its input");
}

void rgb_white_balance_and_saturation_have_numeric_contracts() {
    const auto input = rgb_image(1, {0.2F, 0.4F, 0.6F});
    const std::array warm_white_balance{
        image::AdjustmentNode{
            .node_id = "warm-white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{
                .temperature = 0.75,
            },
        },
    };
    const auto balanced = image::execute_adjustment_nodes(input, warm_white_balance);
    expect(
        balanced.samples[0] / input.samples[0] > balanced.samples[2] / input.samples[2],
        "positive temperature warms processed RGB relative to blue"
    );

    const auto magenta_tint = image::execute_adjustment_nodes(
        rgb_image(1, {0.4F, 0.4F, 0.4F}),
        std::array{image::AdjustmentNode{
            .node_id = "magenta-tint",
            .parameters = image::RgbWhiteBalanceAdjustment{.tint = 0.6},
        }}
    );
    expect(
        magenta_tint.samples[1] < magenta_tint.samples[0]
            && magenta_tint.samples[1] < magenta_tint.samples[2],
        "positive tint moves a neutral sample away from green toward magenta"
    );

    const auto neutral = image::execute_adjustment_nodes(
        input,
        std::array{image::AdjustmentNode{
            .node_id = "neutral-white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{},
        }}
    );
    expect(neutral.samples == input.samples, "neutral RGB white balance is an exact no-op");

    const auto saturation_identity = image::execute_adjustment_nodes(
        input,
        std::array{image::AdjustmentNode{
            .node_id = "neutral-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.0},
        }}
    );
    expect(
        saturation_identity.samples == input.samples,
        "unit saturation is an exact no-op without a perceptual round trip"
    );

    const std::array monochrome{
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.0},
        },
    };
    const auto desaturated = image::execute_adjustment_nodes(input, monochrome);
    expect_close(
        desaturated.samples[0],
        desaturated.samples[1],
        "zero saturation produces an Oklab-neutral working RGB sample"
    );
    expect_close(
        desaturated.samples[1],
        desaturated.samples[2],
        "zero saturation removes chroma without an RGB-luma approximation"
    );

    const std::array boosted{
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 2.0},
        },
    };
    const auto saturated = image::execute_adjustment_nodes(input, boosted);
    expect(saturated.samples != input.samples, "perceptual saturation changes chromatic samples");

    const auto neutral_gray = rgb_image(1, {-0.25F, -0.25F, -0.25F});
    const auto boosted_gray = image::execute_adjustment_nodes(
        neutral_gray,
        std::array{image::AdjustmentNode{
            .node_id = "gray-saturation",
            .parameters = image::SaturationAdjustment{.factor = 4.0},
        }}
    );
    expect(
        boosted_gray.samples == neutral_gray.samples,
        "perceptual saturation preserves the D65 neutral axis exactly, "
        "including negative data"
    );

    const auto extended = image::execute_adjustment_nodes(
        rgb_image(1, {0.1F, 0.4F, 2.0F}),
        std::array{image::AdjustmentNode{
            .node_id = "extended-gamut-saturation",
            .parameters = image::SaturationAdjustment{.factor = 2.0},
        }}
    );
    expect(
        std::ranges::all_of(
            extended.samples,
            [](const float sample) { return std::isfinite(sample); }
        ),
        "perceptual saturation keeps extended-gamut scene-linear output finite"
    );
    expect(
        *std::max_element(extended.samples.begin(), extended.samples.end()) > 1.0F,
        "perceptual saturation does not clip super-white scene-linear output"
    );

    const auto negative_extended = image::execute_adjustment_nodes(
        rgb_image(1, {-0.125F, 0.32F, 1.8F}),
        std::array{image::AdjustmentNode{
            .node_id = "negative-extended-gamut-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.5},
        }}
    );
    expect(
        std::ranges::all_of(
            negative_extended.samples,
            [](const float sample) { return std::isfinite(sample); }
        ),
        "perceptual saturation accepts negative extended-gamut scene-linear "
        "samples"
    );
}

void new_adjustments_respect_node_order() {
    auto input = rgb_image(1, {0.8F, 0.2F, 0.1F});
    input.working_space = linear_srgb();
    const image::AdjustmentNode tone{
        .node_id = "selective-highlights",
        .parameter_schema_version = image::selective_tone_parameter_schema_version,
        .implementation_version = image::selective_tone_implementation_version,
        .parameters = image::SelectiveToneAdjustment{.highlights = 0.8},
    };
    image::PerceptualColorAdjustment color_parameters;
    color_parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 30.0,
        .width_degrees = 180.0,
        .softness = 0.0,
        .lightness = 0.8,
    };
    const image::AdjustmentNode color{
        .node_id = "perceptual-lightness",
        .parameter_schema_version = image::perceptual_color_parameter_schema_version,
        .implementation_version = image::perceptual_color_implementation_version,
        .parameters = color_parameters,
    };
    const std::array tone_then_color{tone, color};
    const std::array color_then_tone{color, tone};
    const auto first = image::execute_adjustment_nodes(input, tone_then_color);
    const auto second = image::execute_adjustment_nodes(input, color_then_tone);
    expect(
        std::abs(first.samples[0] - second.samples[0]) > 1.0e-4F,
        "selective tone and perceptual color execute in declared node order"
    );
}

void node_order_is_observable_and_disabled_nodes_are_skipped() {
    const auto input = rgb_image(1, {0.25F, 0.25F, 0.25F});
    const image::AdjustmentNode exposure{
        .node_id = "exposure",
        .parameters = image::ExposureAdjustment{.stops = 1.0},
    };
    const image::AdjustmentNode contrast{
        .node_id = "contrast",
        .parameters = image::ContrastAdjustment{.factor = 2.0, .pivot = 0.18},
    };
    const std::array exposure_then_contrast{exposure, contrast};
    const std::array contrast_then_exposure{contrast, exposure};
    const auto first = image::execute_adjustment_nodes(input, exposure_then_contrast);
    const auto second = image::execute_adjustment_nodes(input, contrast_then_exposure);
    expect(first.samples[0] > second.samples[0], "contrast consumes the preceding exposure result");
    expect(
        std::abs(first.samples[0] - second.samples[0]) > 0.01F,
        "restrained contrast still observes the declared node order"
    );

    image::AdjustmentNode disabled = exposure;
    disabled.enabled = false;
    const std::array disabled_only{disabled};
    const auto unchanged = image::execute_adjustment_nodes(input, disabled_only);
    expect_close(unchanged.samples[0], input.samples[0], "disabled nodes do not affect pixels");
}

void image_completion_is_bounded_and_detail_tile_equivalent() {
    auto input = rgb_raster(4U, 4U, std::vector<float>(4U * 4U * 3U, 0.2F));
    input.working_space = linear_srgb();
    const std::vector<std::uint8_t> replacement{
        255U,
        0U,
        0U,
        255U,
        255U,
        0U,
        0U,
        255U,
        255U,
        0U,
        0U,
        255U,
        255U,
        0U,
        0U,
        255U,
    };
    const image::AdjustmentNode completion{
        .node_id = "image-completion",
        .parameters = image::ImageCompletionAdjustment{
            .patches = {
                image::ImageCompletionPatch{
                    .raster_width = 2U,
                    .raster_height = 2U,
                    .coordinate_width = 4U,
                    .coordinate_height = 4U,
                    .bounds_left = 0.25,
                    .bounds_top = 0.25,
                    .bounds_right = 0.75,
                    .bounds_bottom = 0.75,
                    .strength = 0.5,
                    .rgba8 = replacement,
                },
            }
        },
    };
    const auto full = image::execute_adjustment_nodes(input, std::array{completion});
    const auto offset = [](const std::uint32_t x, const std::uint32_t y) {
        return (static_cast<std::size_t>(y) * 4U + x) * 3U;
    };
    expect_close(
        full.samples[offset(0U, 0U)],
        0.2F,
        "image completion preserves pixels outside accepted bounds"
    );
    expect_close(
        full.samples[offset(3U, 3U) + 2U],
        0.2F,
        "image completion leaves the opposite outside corner unchanged"
    );
    expect_close(
        full.samples[offset(1U, 1U)],
        0.6F,
        "image completion blends decoded linear red by node strength"
    );
    expect_close(
        full.samples[offset(1U, 1U) + 1U],
        0.1F,
        "image completion blends non-red channels by node strength"
    );

    auto tile = rgb_raster(2U, 2U, std::vector<float>(2U * 2U * 3U, 0.2F));
    tile.working_space = linear_srgb();
    const auto rendered_tile = image::execute_adjustment_nodes(
        tile,
        std::array{completion},
        image::AdjustmentExecutionContext{
            .origin_x = 1U,
            .origin_y = 1U,
            .full_dimensions = {4U, 4U},
        }
    );
    for (std::uint32_t y = 0U; y < 2U; ++y) {
        for (std::uint32_t x = 0U; x < 2U; ++x) {
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const std::size_t tile_offset =
                    (static_cast<std::size_t>(y) * 2U + x) * 3U + channel;
                expect_close(
                    rendered_tile.samples[tile_offset],
                    full.samples[offset(x + 1U, y + 1U) + channel],
                    "image completion detail tiles match the full render"
                );
            }
        }
    }
}

} // namespace

int main() {
    exposure_preserves_unclipped_scene_range_and_padding();
    rgb_white_balance_and_saturation_have_numeric_contracts();
    new_adjustments_respect_node_order();
    node_order_is_observable_and_disabled_nodes_are_skipped();
    image_completion_is_bounded_and_detail_tile_equivalent();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

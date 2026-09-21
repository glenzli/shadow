#include "adjustment_execution_contract_cases.hpp"
#include "advanced_operation_fixture.hpp"
#include "execution_parity_fixture.hpp"
#include "perceptual_operation_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/display_output.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract {

namespace {

using advanced_fixture::advanced_test_lut;
using parity_fixture::close_to_cpu;
using parity_fixture::make_image;
using parity_fixture::maximum_rgb8_difference;
using perceptual_fixture::combined_perceptual_parameters;
using perceptual_fixture::ordered_point_parameters;
using perceptual_fixture::perceptual_mapping_parameters;
using perceptual_fixture::perceptual_node;
using perceptual_fixture::primary_point_parameters;
using perceptual_fixture::selective_color_parameters;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::array<image::AdjustmentNode, 3U>
advanced_nodes(const double curve_midpoint = 0.76, const double lut_intensity = 0.64) {
    return {
        image::AdjustmentNode{
            .node_id = "oklab-lightness-curve",
            .parameter_schema_version = image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version = image::oklab_lightness_tone_curve_implementation_version,
            .parameters =
                image::OklabLightnessToneCurve{
                    .lightness =
                        {
                            .points =
                                {
                                    {0.0, 0.0},
                                    {0.18, 0.11},
                                    {0.62, curve_midpoint},
                                    {1.0, 1.08},
                                },
                        },
                },
        },
        image::AdjustmentNode{
            .node_id = "pure-color-grading",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                    .shadows_hue = 28.0,
                    .shadows_saturation = 0.32,
                    .shadows_luminance = -0.16,
                    .midtones_hue = 118.0,
                    .midtones_saturation = 0.20,
                    .midtones_luminance = 0.08,
                    .highlights_hue = 248.0,
                    .highlights_saturation = 0.38,
                    .highlights_luminance = 0.18,
                    .grading_blending = 0.66,
                    .grading_balance = -0.24,
                },
        },
        image::AdjustmentNode{
            .node_id = "cube-lut",
            .parameters = image::CubeLutAdjustment{
                .lut = advanced_test_lut(),
                .intensity = lut_intensity,
            },
        },
    };
}

void advanced_pixel_local_operations_match_the_cpu_oracle() {
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        return;
    }
    const auto input = make_image(41U, 27U, true);
    const auto nodes = advanced_nodes();
    double worst_error = 0.0;
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const std::span<const image::AdjustmentNode> isolated{
            nodes.data() + index,
            1U,
        };
        const auto cpu = image::execute_adjustment_nodes(input, isolated);
        const auto metal = image::execute_adjustment_nodes_with_backend(
            input,
            isolated,
            {},
            image::AdjustmentBackendMode::metal
        );
        double error = 0.0;
        const bool parity = close_to_cpu(metal.pixels, cpu, error, 4.0e-5);
        if (!parity) {
            std::cerr << "Advanced standalone parity operation=" << index << " max=" << error
                      << '\n';
        }
        expect(
            metal.backend == image::AdjustmentBackend::metal && !metal.fell_back && parity,
            "each advanced pixel-local operation matches the CPU oracle on Metal"
        );
        worst_error = std::max(worst_error, error);
    }

    const auto cpu = image::execute_adjustment_nodes(input, nodes);
    const auto metal = image::execute_adjustment_nodes_with_backend(
        input,
        nodes,
        {},
        image::AdjustmentBackendMode::metal
    );
    double combined_error = 0.0;
    const bool combined_parity = close_to_cpu(metal.pixels, cpu, combined_error, 8.0e-5);
    if (!combined_parity) {
        std::cerr << "Advanced standalone combined parity max=" << combined_error << '\n';
    }
    expect(
        metal.backend == image::AdjustmentBackend::metal && !metal.fell_back && combined_parity,
        "Oklab curve, pure color grading, and LUT preserve CPU order and semantics on Metal"
    );
    worst_error = std::max(worst_error, combined_error);

    std::vector<image::ToneCurvePoint> dense_points;
    dense_points.reserve(image::maximum_tone_curve_points);
    for (std::size_t index = 0U; index < image::maximum_tone_curve_points; ++index) {
        const double x =
            static_cast<double>(index) / static_cast<double>(image::maximum_tone_curve_points - 1U);
        dense_points.push_back({
            .x = x,
            .y = x + 0.04 * x * (1.0 - x),
        });
    }
    const std::array dense_curve{
        image::AdjustmentNode{
            .node_id = "maximum-density-oklab-curve",
            .parameter_schema_version = image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version = image::oklab_lightness_tone_curve_implementation_version,
            .parameters = image::OklabLightnessToneCurve{
                .lightness = {.points = std::move(dense_points)},
            },
        },
    };
    const auto dense_cpu = image::execute_adjustment_nodes(input, dense_curve);
    const auto dense_metal = image::execute_adjustment_nodes_with_backend(
        input,
        dense_curve,
        {},
        image::AdjustmentBackendMode::metal
    );
    double dense_error = 0.0;
    expect(
        dense_metal.backend == image::AdjustmentBackend::metal && !dense_metal.fell_back
            && close_to_cpu(dense_metal.pixels, dense_cpu, dense_error, 8.0e-5),
        "a maximum-density 256-point curve remains eligible and CPU-equivalent on Metal"
    );
    worst_error = std::max(worst_error, dense_error);
    std::cout << "Metal advanced-operation maximum absolute error: " << worst_error << '\n';
}

void perceptual_color_matches_cpu_and_display_oracles_on_metal() {
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        return;
    }
    const auto input = make_image(43U, 29U, true);
    double worst_linear_error = 0.0;
    std::uint8_t worst_display_error = 0U;
    const auto verify = [&input, &worst_linear_error, &worst_display_error](
                            const std::span<const image::AdjustmentNode> nodes,
                            const std::string_view description
                        ) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {.full_dimensions = input.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto metal = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {.full_dimensions = input.dimensions},
            image::AdjustmentBackendMode::metal
        );
        double linear_error = 0.0;
        const bool linear_parity = close_to_cpu(metal.pixels, cpu.pixels, linear_error, 2.0e-4);
        const auto cpu_display = image::render_linear_srgb_to_display_srgb8_cpu_reference(
            cpu.pixels,
            {.target_dimensions = input.dimensions}
        );
        const auto metal_display = image::render_linear_srgb_to_display_srgb8_cpu_reference(
            metal.pixels,
            {.target_dimensions = input.dimensions}
        );
        const std::uint8_t display_error =
            maximum_rgb8_difference(metal_display.bytes, cpu_display.bytes);
        if (!linear_parity || display_error > 1U) {
            std::cerr << "Perceptual Metal parity " << description
                      << ": linear max=" << linear_error
                      << ", display max=" << static_cast<unsigned int>(display_error) << '\n';
        }
        expect(
            metal.backend == image::AdjustmentBackend::metal && !metal.fell_back && linear_parity,
            description
        );
        expect(
            display_error <= 1U,
            "PerceptualColor Metal output remains within one display RGB8 code"
        );
        worst_linear_error = std::max(worst_linear_error, linear_error);
        worst_display_error = std::max(worst_display_error, display_error);
        return cpu.pixels;
    };

    const std::array isolated{
        perceptual_node("perceptual-mixer", perceptual_mapping_parameters()),
        perceptual_node("primary-point-color", primary_point_parameters()),
        perceptual_node("ordered-point-color", ordered_point_parameters()),
        perceptual_node("selective-color", selective_color_parameters()),
        perceptual_node("combined-perceptual-color", combined_perceptual_parameters()),
        perceptual_node(
            "global-oklab-opponent-balance",
            image::PerceptualColorAdjustment{
                .global_a_balance = 0.43,
                .global_b_balance = -0.31,
            }
        ),
    };
    for (std::size_t index = 0U; index < isolated.size(); ++index) {
        verify(
            {isolated.data() + index, 1U},
            "an isolated PerceptualColor sub-stage matches the CPU oracle on forced Metal"
        );
    }

    image::RgbToneCurves rgb_curves;
    rgb_curves.channels[0].points = {{0, 0.01}, {0.3, 0.24}, {0.7, 0.77}, {1, 1.08}};
    rgb_curves.channels[1].points = {{0, 0}, {0.45, 0.5}, {1, 1}};
    rgb_curves.channels[2].points = {{0, -0.02}, {0.5, 0.49}, {1, 0.97}};
    rgb_curves.channels[3].points = {{0, 0.04}, {0.6, 0.57}, {1, 1.02}};
    const std::array rgb_nodes{
        image::AdjustmentNode{.node_id = "rgb-curves", .parameters = rgb_curves}
    };
    verify(
        rgb_nodes,
        "RGB master and channel curves match CPU on forced Metal including extended range"
    );

    const auto advanced = advanced_nodes();
    const std::array combined{
        image::AdjustmentNode{
            .node_id = "pre-perceptual-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.42},
        },
        perceptual_node("combined-perceptual-between-nodes", combined_perceptual_parameters()),
        advanced[0],
        rgb_nodes[0],
        advanced[2],
    };
    verify(
        combined,
        "PerceptualColor preserves declared order with exposure, Oklab curve, and LUT"
    );

    const image::AdjustmentNode exposure{
        .node_id = "order-exposure",
        .parameters = image::ExposureAdjustment{.stops = 0.85},
    };
    const auto perceptual = perceptual_node("order-perceptual", combined_perceptual_parameters());
    const std::array exposure_then_perceptual{exposure, perceptual};
    const std::array perceptual_then_exposure{perceptual, exposure};
    const auto first_cpu = verify(
        exposure_then_perceptual,
        "exposure followed by PerceptualColor matches CPU on forced Metal"
    );
    const auto second_cpu = verify(
        perceptual_then_exposure,
        "PerceptualColor followed by exposure matches CPU on forced Metal"
    );
    expect(
        first_cpu.samples != second_cpu.samples,
        "PerceptualColor and neighboring pixel-local nodes retain observable source order"
    );

    std::cout << "Metal PerceptualColor maximum absolute error: " << worst_linear_error
              << "; display RGB8 max=" << static_cast<unsigned int>(worst_display_error) << '\n';
}
} // namespace

int run_advanced_pixel_local_operations_match_the_cpu_oracle() {
    failures = 0;
    advanced_pixel_local_operations_match_the_cpu_oracle();
    return failures;
}

int run_perceptual_color_matches_cpu_and_display_oracles_on_metal() {
    failures = 0;
    perceptual_color_matches_cpu_and_display_oracles_on_metal();
    return failures;
}

} // namespace shadow::image::adjustment_execution_contract

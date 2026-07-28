#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::warm_edit_gpu_contract {

namespace {

using parity_fixture::linear_close;
using parity_fixture::make_random_image;
using parity_fixture::rgb8_difference;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::CubeLut3D advanced_test_lut() {
    image::CubeLut3D lut{
        .title = "Warm GPU advanced contract",
        .size = 3U,
        .domain_min = {-0.25, -0.10, -0.20},
        .domain_max = {1.50, 1.30, 1.70},
    };
    lut.entries.reserve(27U);
    for (std::uint16_t blue = 0U; blue < lut.size; ++blue) {
        for (std::uint16_t green = 0U; green < lut.size; ++green) {
            for (std::uint16_t red = 0U; red < lut.size; ++red) {
                const float r = static_cast<float>(red) / 2.0F;
                const float g = static_cast<float>(green) / 2.0F;
                const float b = static_cast<float>(blue) / 2.0F;
                lut.entries.push_back({
                    0.05F + 0.78F * r + 0.12F * g,
                    0.03F + 0.82F * g + 0.10F * b,
                    0.02F + 0.14F * r + 0.76F * b,
                });
            }
        }
    }
    return lut;
}

[[nodiscard]] std::array<image::AdjustmentNode, 3U> advanced_nodes(
    const double curve_midpoint = 0.76,
    const double lut_intensity = 0.64
) {
    return {
        image::AdjustmentNode{
            .node_id = "oklab-lightness-curve",
            .parameter_schema_version =
                image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version =
                image::oklab_lightness_tone_curve_implementation_version,
            .parameters = image::OklabLightnessToneCurve{
                .lightness = {
                    .points = {
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
            .parameter_schema_version =
                image::detail_effects_parameter_schema_version,
            .implementation_version =
                image::color_grading_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass =
                    image::DetailEffectsExecutionPass::color_grading,
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

[[nodiscard]] std::array<image::AdjustmentNode, 1U> perceptual_nodes() {
    image::PerceptualColorAdjustment parameters;
    parameters.vibrance = 0.31;
    parameters.hue = {0.18, -0.12, 0.07, -0.09, 0.14, -0.16, 0.11, -0.06};
    parameters.saturation = {
        0.13,
        -0.08,
        0.05,
        0.11,
        -0.06,
        0.15,
        -0.09,
        0.07,
    };
    parameters.lightness = {
        -0.06,
        0.09,
        -0.04,
        0.07,
        -0.08,
        0.05,
        -0.03,
        0.10,
    };
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 26.0,
        .width_degrees = 52.0,
        .softness = 0.42,
        .hue_shift_degrees = 14.0,
        .saturation = 0.22,
        .lightness = -0.12,
    };
    parameters.additional_color_ranges = {
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 44.0,
            .width_degrees = 68.0,
            .softness = 0.36,
            .hue_shift_degrees = 31.0,
            .saturation = 0.17,
            .lightness = -0.07,
        },
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 92.0,
            .width_degrees = 57.0,
            .softness = 0.51,
            .hue_shift_degrees = -16.0,
            .saturation = -0.14,
            .lightness = 0.11,
        },
    };
    parameters.selective_color_relative = false;
    parameters.selective_color_lightness_protection = 0.67;
    parameters.selective_color_cmyk = {{
        {{0.10, -0.15, 0.06, 0.04}},
        {{-0.07, 0.12, 0.04, -0.03}},
        {{0.06, -0.05, 0.13, 0.02}},
        {{-0.09, 0.06, -0.11, 0.05}},
        {{0.12, 0.03, -0.07, -0.02}},
        {{-0.04, 0.14, 0.07, 0.03}},
        {{0.02, -0.02, 0.03, 0.07}},
        {{-0.03, 0.04, -0.02, 0.05}},
        {{0.02, -0.01, 0.02, -0.10}},
    }};
    return {
        image::AdjustmentNode{
            .node_id = "warm-perceptual-color",
            .parameter_schema_version =
                image::perceptual_color_parameter_schema_version,
            .implementation_version =
                image::perceptual_color_implementation_version,
            .parameters = std::move(parameters),
        },
    };
}

[[nodiscard]] std::array<image::AdjustmentNode, 1U> color_warper_nodes() {
    image::OklabColorWarperAdjustment parameters;
    for (std::size_t row = 0U; row < image::oklab_color_warper_grid_side; ++row) {
        for (std::size_t column = 0U;
             column < image::oklab_color_warper_grid_side;
             ++column) {
            auto& point = parameters.control_points[
                row * image::oklab_color_warper_grid_side + column
            ];
            point.a_offset = 0.008 * static_cast<double>(column) - 0.016;
            point.b_offset = 0.007 * static_cast<double>(row) - 0.014;
        }
    }
    parameters.strength = 0.73;
    return {
        image::AdjustmentNode{
            .node_id = "warm-oklab-color-warper",
            .parameter_schema_version = image::oklab_color_warper_parameter_schema_version,
            .implementation_version = image::oklab_color_warper_implementation_version,
            .parameters = std::move(parameters),
        },
    };
}

void advanced_resources_match_cpu_and_reuse_side_table_uploads() {
    const auto source = make_random_image(137U, 83U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "advanced resident Metal was required but could not be prepared"
        );
        return;
    }

    const auto initial = preparation.session->stats();
    expect(
        initial.curve_resource_upload_count == 0U
            && initial.lut_resource_upload_count == 0U
            && initial.resource_cache_hit_count == 0U,
        "a new warm session has an empty advanced-resource cache"
    );
    const auto render_and_compare = [&source, &preparation](
        const std::span<const image::AdjustmentNode> nodes,
        const std::string_view description
    ) {
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_with_backend(
                cpu.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
        const auto gpu = preparation.session->render(nodes, plan, true);
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            std::cerr << "Advanced warm render failed: " << gpu.diagnostic << '\n';
            expect(false, description);
            return;
        }
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            8.0e-5
        );
        const auto display_difference =
            rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
        if (!linear_parity) {
            std::cerr << "Advanced warm linear parity max="
                      << maximum_error << '\n';
        }
        expect(
            linear_parity
                && display_difference.maximum <= 1U
                && display_difference.p99 <= 1U,
            description
        );
    };

    auto nodes = advanced_nodes();
    render_and_compare(nodes, "the first advanced warm render matches the CPU oracle");
    const auto after_first = preparation.session->stats();
    expect(
        after_first.curve_resource_upload_count
                == initial.curve_resource_upload_count + 1U
            && after_first.lut_resource_upload_count
                == initial.lut_resource_upload_count + 1U
            && after_first.resource_cache_hit_count
                == initial.resource_cache_hit_count
            && after_first.gpu_buffer_allocation_count
                == initial.gpu_buffer_allocation_count + 2U
            && after_first.resident_bytes > initial.resident_bytes,
        "the first curve and LUT each publish one resident side-table buffer"
    );

    render_and_compare(nodes, "an identical advanced warm render remains CPU-equivalent");
    const auto after_identical = preparation.session->stats();
    expect(
        after_identical.curve_resource_upload_count
                == after_first.curve_resource_upload_count
            && after_identical.lut_resource_upload_count
                == after_first.lut_resource_upload_count
            && after_identical.resource_cache_hit_count
                == after_first.resource_cache_hit_count + 2U
            && after_identical.gpu_buffer_allocation_count
                == after_first.gpu_buffer_allocation_count
            && after_identical.resident_bytes == after_first.resident_bytes,
        "an identical curve and LUT hit both caches without another upload"
    );

    std::get<image::CubeLutAdjustment>(nodes[2U].parameters).intensity = 0.31;
    render_and_compare(nodes, "changing LUT intensity remains CPU-equivalent");
    const auto after_intensity = preparation.session->stats();
    expect(
        after_intensity.curve_resource_upload_count
                == after_identical.curve_resource_upload_count
            && after_intensity.lut_resource_upload_count
                == after_identical.lut_resource_upload_count
            && after_intensity.resource_cache_hit_count
                == after_identical.resource_cache_hit_count + 2U
            && after_intensity.gpu_buffer_allocation_count
                == after_identical.gpu_buffer_allocation_count
            && after_intensity.resident_bytes == after_identical.resident_bytes,
        "LUT intensity is an operation scalar and does not re-upload either side table"
    );

    std::get<image::OklabLightnessToneCurve>(nodes[0U].parameters)
        .lightness.points[2U].y = 0.68;
    render_and_compare(nodes, "changing the Oklab curve remains CPU-equivalent");
    const auto after_curve = preparation.session->stats();
    expect(
        after_curve.curve_resource_upload_count
                == after_intensity.curve_resource_upload_count + 1U
            && after_curve.lut_resource_upload_count
                == after_intensity.lut_resource_upload_count
            && after_curve.resource_cache_hit_count
                == after_intensity.resource_cache_hit_count + 1U
            && after_curve.gpu_buffer_allocation_count
                == after_intensity.gpu_buffer_allocation_count + 1U
            && after_curve.resident_bytes > after_intensity.resident_bytes,
        "changing only the curve publishes one curve buffer and reuses the LUT"
    );
}

void perceptual_resources_match_cpu_and_have_independent_caches() {
    const auto source = make_random_image(139U, 87U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "perceptual resident Metal was required but could not be prepared"
        );
        return;
    }

    const auto initial = preparation.session->stats();
    expect(
        initial.perceptual_mixer_resource_upload_count == 0U
            && initial.perceptual_range_resource_upload_count == 0U
            && initial.selective_color_resource_upload_count == 0U
            && initial.resource_cache_hit_count == 0U,
        "a new warm session has empty perceptual-color resource caches"
    );
    const auto render_and_compare = [&source, &preparation](
        const std::span<const image::AdjustmentNode> nodes,
        const std::string_view description
    ) {
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_with_backend(
                cpu.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
        const auto gpu = preparation.session->render(nodes, plan, true);
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            std::cerr << "Perceptual warm render failed: "
                      << gpu.diagnostic << '\n';
            expect(false, description);
            return;
        }
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            2.0e-4
        );
        const auto display_difference =
            rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
        if (!linear_parity || display_difference.maximum > 1U) {
            std::cerr << "Perceptual warm parity: linear max="
                      << maximum_error << ", display max="
                      << static_cast<unsigned int>(display_difference.maximum)
                      << '\n';
        }
        expect(
            linear_parity
                && display_difference.maximum <= 1U
                && display_difference.p99 <= 1U,
            description
        );
    };

    auto nodes = perceptual_nodes();
    render_and_compare(
        nodes,
        "combined perceptual mapping, Point Color, and Selective Color match CPU"
    );
    const auto after_first = preparation.session->stats();
    expect(
        after_first.perceptual_mixer_resource_upload_count
                == initial.perceptual_mixer_resource_upload_count + 1U
            && after_first.perceptual_range_resource_upload_count
                == initial.perceptual_range_resource_upload_count + 1U
            && after_first.selective_color_resource_upload_count
                == initial.selective_color_resource_upload_count + 1U
            && after_first.resource_cache_hit_count
                == initial.resource_cache_hit_count
            && after_first.gpu_buffer_allocation_count
                == initial.gpu_buffer_allocation_count + 3U
            && after_first.resident_bytes > initial.resident_bytes,
        "the first perceptual render publishes one buffer per resource family"
    );

    render_and_compare(nodes, "an identical perceptual render remains CPU-equivalent");
    const auto after_identical = preparation.session->stats();
    expect(
        after_identical.perceptual_mixer_resource_upload_count
                == after_first.perceptual_mixer_resource_upload_count
            && after_identical.perceptual_range_resource_upload_count
                == after_first.perceptual_range_resource_upload_count
            && after_identical.selective_color_resource_upload_count
                == after_first.selective_color_resource_upload_count
            && after_identical.resource_cache_hit_count
                == after_first.resource_cache_hit_count + 3U
            && after_identical.gpu_buffer_allocation_count
                == after_first.gpu_buffer_allocation_count
            && after_identical.resident_bytes == after_first.resident_bytes,
        "an identical perceptual render hits all three immutable caches"
    );

    auto& parameters =
        std::get<image::PerceptualColorAdjustment>(nodes[0U].parameters);
    parameters.vibrance = 0.47;
    parameters.color_range.center_degrees = 31.0;
    parameters.selective_color_relative = true;
    parameters.selective_color_lightness_protection = 0.48;
    render_and_compare(nodes, "changing only perceptual scalars remains CPU-equivalent");
    const auto after_scalars = preparation.session->stats();
    expect(
        after_scalars.perceptual_mixer_resource_upload_count
                == after_identical.perceptual_mixer_resource_upload_count
            && after_scalars.perceptual_range_resource_upload_count
                == after_identical.perceptual_range_resource_upload_count
            && after_scalars.selective_color_resource_upload_count
                == after_identical.selective_color_resource_upload_count
            && after_scalars.resource_cache_hit_count
                == after_identical.resource_cache_hit_count + 3U
            && after_scalars.gpu_buffer_allocation_count
                == after_identical.gpu_buffer_allocation_count,
        "vibrance, primary range, relative mode, and L protection are operation scalars"
    );

    parameters.hue[3U] += 0.04;
    render_and_compare(nodes, "changing one mixer band remains CPU-equivalent");
    const auto after_mixer = preparation.session->stats();
    expect(
        after_mixer.perceptual_mixer_resource_upload_count
                == after_scalars.perceptual_mixer_resource_upload_count + 1U
            && after_mixer.perceptual_range_resource_upload_count
                == after_scalars.perceptual_range_resource_upload_count
            && after_mixer.selective_color_resource_upload_count
                == after_scalars.selective_color_resource_upload_count
            && after_mixer.resource_cache_hit_count
                == after_scalars.resource_cache_hit_count + 2U
            && after_mixer.gpu_buffer_allocation_count
                == after_scalars.gpu_buffer_allocation_count + 1U,
        "changing one color-mixer band uploads only the mixer table"
    );

    parameters.additional_color_ranges[1U].lightness = 0.08;
    render_and_compare(nodes, "changing one ordered Point Color range remains CPU-equivalent");
    const auto after_range = preparation.session->stats();
    expect(
        after_range.perceptual_mixer_resource_upload_count
                == after_mixer.perceptual_mixer_resource_upload_count
            && after_range.perceptual_range_resource_upload_count
                == after_mixer.perceptual_range_resource_upload_count + 1U
            && after_range.selective_color_resource_upload_count
                == after_mixer.selective_color_resource_upload_count
            && after_range.resource_cache_hit_count
                == after_mixer.resource_cache_hit_count + 2U
            && after_range.gpu_buffer_allocation_count
                == after_mixer.gpu_buffer_allocation_count + 1U,
        "changing an additional Point Color range uploads only the range table"
    );

    parameters.selective_color_cmyk[4U][2U] -= 0.05;
    render_and_compare(nodes, "changing one Selective Color value remains CPU-equivalent");
    const auto after_selective = preparation.session->stats();
    expect(
        after_selective.perceptual_mixer_resource_upload_count
                == after_range.perceptual_mixer_resource_upload_count
            && after_selective.perceptual_range_resource_upload_count
                == after_range.perceptual_range_resource_upload_count
            && after_selective.selective_color_resource_upload_count
                == after_range.selective_color_resource_upload_count + 1U
            && after_selective.resource_cache_hit_count
                == after_range.resource_cache_hit_count + 2U
            && after_selective.gpu_buffer_allocation_count
                == after_range.gpu_buffer_allocation_count + 1U,
        "changing the CMYK grid uploads only the Selective Color table"
    );
}

void color_warper_matches_cpu_and_reuses_its_resident_table() {
    const auto source = make_random_image(143U, 89U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "Color Warper resident Metal was required but could not be prepared"
        );
        return;
    }

    const auto render_and_compare = [&source, &preparation](
        const std::span<const image::AdjustmentNode> nodes,
        const std::string_view description
    ) {
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto gpu = preparation.session->render(nodes, plan, true);
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            std::cerr << "Color Warper warm render failed: "
                      << gpu.diagnostic << '\n';
            expect(false, description);
            return;
        }
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            2.0e-4
        );
        if (!linear_parity) {
            std::cerr << "Color Warper warm parity: linear max="
                      << maximum_error << '\n';
        }
        expect(linear_parity, description);
    };

    auto nodes = color_warper_nodes();
    const auto initial = preparation.session->stats();
    render_and_compare(nodes, "resident Color Warper matches the CPU lattice oracle");
    const auto after_first = preparation.session->stats();
    expect(
        after_first.perceptual_mixer_resource_upload_count
                == initial.perceptual_mixer_resource_upload_count + 1U
            && after_first.gpu_buffer_allocation_count
                == initial.gpu_buffer_allocation_count + 1U,
        "Color Warper publishes one immutable control lattice in the existing pixel-local table"
    );

    render_and_compare(nodes, "an identical Color Warper reuses its resident lattice");
    const auto after_identical = preparation.session->stats();
    expect(
        after_identical.perceptual_mixer_resource_upload_count
                == after_first.perceptual_mixer_resource_upload_count
            && after_identical.resource_cache_hit_count
                == after_first.resource_cache_hit_count + 1U
            && after_identical.gpu_buffer_allocation_count
                == after_first.gpu_buffer_allocation_count,
        "an unchanged Color Warper control lattice hits its resident resource cache"
    );

    auto& parameters = std::get<image::OklabColorWarperAdjustment>(nodes[0U].parameters);
    parameters.control_points[12U].a_offset += 0.011;
    render_and_compare(nodes, "a changed Color Warper lattice remains CPU-equivalent");
    const auto after_changed = preparation.session->stats();
    expect(
        after_changed.perceptual_mixer_resource_upload_count
                == after_identical.perceptual_mixer_resource_upload_count + 1U
            && after_changed.gpu_buffer_allocation_count
                == after_identical.gpu_buffer_allocation_count + 1U,
        "changing Color Warper geometry uploads only its replacement lattice"
    );
}

} // namespace

int run_advanced_resource_cache_contract() {
    failures = 0;
    advanced_resources_match_cpu_and_reuse_side_table_uploads();
    return failures;
}

int run_perceptual_resource_cache_contract() {
    failures = 0;
    perceptual_resources_match_cpu_and_have_independent_caches();
    return failures;
}

int run_color_warper_resource_cache_contract() {
    failures = 0;
    color_warper_matches_cpu_and_reuses_its_resident_table();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

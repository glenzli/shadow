#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::ScopedEnvironment;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] image::FloatRgbImage make_image(
    const std::uint32_t width,
    const std::uint32_t height,
    const bool padded = false
) {
    const std::size_t active_row = static_cast<std::size_t>(width) * 3U;
    const std::size_t row_floats = active_row + (padded ? 5U : 0U);
    image::FloatRgbImage result{
        .dimensions = {width, height},
        .row_stride_bytes = row_floats * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_srgb(),
        .samples = std::vector<float>(row_floats * height, 17.0F),
    };
    std::mt19937 generator(0x5a17U);
    std::uniform_real_distribution<float> samples(-0.15F, 2.25F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * row_floats;
        for (std::size_t index = 0U; index < active_row; ++index) {
            result.samples[row + index] = samples(generator);
        }
    }
    if (width > 1U && height > 0U) {
        result.samples[0] = -0.05F;
        result.samples[1] = 0.20F;
        result.samples[2] = 1.75F;
        result.samples[3] = 2.0F;
        result.samples[4] = 2.0F;
        result.samples[5] = 2.0F;
    }
    return result;
}

[[nodiscard]] std::array<image::AdjustmentNode, 4U> core_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{
                .temperature = 0.24,
                .tint = -0.13,
            },
        },
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.37},
        },
        image::AdjustmentNode{
            .node_id = "contrast",
            .parameters = image::ContrastAdjustment{
                .factor = 1.42,
                .pivot = 0.18,
            },
        },
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.76},
        },
    };
}

[[nodiscard]] image::CubeLut3D advanced_test_lut() {
    image::CubeLut3D lut{
        .title = "GPU advanced contract",
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

[[nodiscard]] image::PerceptualColorAdjustment perceptual_mapping_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.vibrance = 0.38;
    parameters.hue = {0.22, -0.16, 0.08, -0.12, 0.18, -0.20, 0.14, -0.09};
    parameters.saturation = {0.15, -0.10, 0.07, 0.13, -0.08, 0.17, -0.12, 0.09};
    parameters.lightness = {-0.08, 0.11, -0.06, 0.09, -0.10, 0.07, -0.05, 0.12};
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment primary_point_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 28.0,
        .width_degrees = 54.0,
        .softness = 0.45,
        .hue_shift_degrees = 17.0,
        .saturation = 0.28,
        .lightness = -0.16,
    };
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment ordered_point_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.additional_color_ranges = {
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 30.0,
            .width_degrees = 75.0,
            .softness = 0.35,
            .hue_shift_degrees = 46.0,
            .saturation = 0.21,
            .lightness = -0.08,
        },
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 82.0,
            .width_degrees = 62.0,
            .softness = 0.55,
            .hue_shift_degrees = -19.0,
            .saturation = -0.17,
            .lightness = 0.14,
        },
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 318.0,
            .width_degrees = 48.0,
            .softness = 0.30,
            .hue_shift_degrees = 11.0,
            .saturation = 0.09,
            .lightness = 0.06,
        },
    };
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment selective_color_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.selective_color_relative = false;
    parameters.selective_color_lightness_protection = 0.72;
    parameters.selective_color_cmyk = {{
        {{0.12, -0.18, 0.07, 0.05}},
        {{-0.09, 0.14, 0.05, -0.04}},
        {{0.08, -0.06, 0.16, 0.03}},
        {{-0.11, 0.07, -0.13, 0.06}},
        {{0.15, 0.04, -0.08, -0.03}},
        {{-0.05, 0.17, 0.09, 0.04}},
        {{0.03, -0.02, 0.04, 0.08}},
        {{-0.04, 0.05, -0.03, 0.06}},
        {{0.02, -0.01, 0.03, -0.12}},
    }};
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment combined_perceptual_parameters() {
    auto parameters = perceptual_mapping_parameters();
    parameters.color_range = primary_point_parameters().color_range;
    parameters.additional_color_ranges =
        ordered_point_parameters().additional_color_ranges;
    const auto selective = selective_color_parameters();
    parameters.selective_color_relative = selective.selective_color_relative;
    parameters.selective_color_lightness_protection =
        selective.selective_color_lightness_protection;
    parameters.selective_color_cmyk = selective.selective_color_cmyk;
    return parameters;
}

[[nodiscard]] image::AdjustmentNode perceptual_node(
    std::string id,
    image::PerceptualColorAdjustment parameters
) {
    return image::AdjustmentNode{
        .node_id = std::move(id),
        .parameter_schema_version =
            image::perceptual_color_parameter_schema_version,
        .implementation_version =
            image::perceptual_color_implementation_version,
        .parameters = std::move(parameters),
    };
}

[[nodiscard]] image::OklabColorWarperAdjustment color_warper_parameters() {
    image::OklabColorWarperAdjustment parameters;
    for (auto& point : parameters.control_points) {
        point.a_offset = 0.042;
        point.b_offset = -0.028;
    }
    parameters.strength = 0.72;
    return parameters;
}

[[nodiscard]] image::AdjustmentNode color_warper_node(
    std::string id,
    image::OklabColorWarperAdjustment parameters
) {
    return image::AdjustmentNode{
        .node_id = std::move(id),
        .parameter_schema_version = image::oklab_color_warper_parameter_schema_version,
        .implementation_version = image::oklab_color_warper_implementation_version,
        .parameters = std::move(parameters),
    };
}

[[nodiscard]] bool close_to_cpu(
    const image::FloatRgbImage& actual,
    const image::FloatRgbImage& expected,
    double& maximum_error,
    const double relative_tolerance = 3.0e-5
) {
    if (actual.dimensions != expected.dimensions
        || actual.row_stride_bytes != expected.row_stride_bytes
        || actual.samples.size() != expected.samples.size()) {
        return false;
    }
    maximum_error = 0.0;
    for (std::size_t index = 0U; index < expected.samples.size(); ++index) {
        const double reference = expected.samples[index];
        const double difference = std::abs(
            static_cast<double>(actual.samples[index]) - reference
        );
        maximum_error = std::max(maximum_error, difference);
        const double tolerance =
            relative_tolerance * std::max(1.0, std::abs(reference));
        if (difference > tolerance) {
            std::cerr << "Parity mismatch at " << index << ": actual="
                      << actual.samples[index] << ", CPU=" << expected.samples[index]
                      << ", tolerance=" << tolerance << '\n';
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint8_t maximum_rgb8_difference(
    const std::span<const std::uint8_t> actual,
    const std::span<const std::uint8_t> expected
) {
    if (actual.size() != expected.size()) {
        return std::numeric_limits<std::uint8_t>::max();
    }
    std::uint8_t maximum = 0U;
    for (std::size_t index = 0U; index < actual.size(); ++index) {
        maximum = std::max(
            maximum,
            static_cast<std::uint8_t>(std::abs(
                static_cast<int>(actual[index])
                - static_cast<int>(expected[index])
            ))
        );
    }
    return maximum;
}

void neutral_and_disabled_plans_have_no_backend_route() {
    const auto input = make_image(9U, 7U, true);
    std::array nodes{
        image::AdjustmentNode{
            .node_id = "neutral-exposure",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "disabled-active-curve",
            .enabled = false,
            .parameters = image::OklabLightnessToneCurve{
                .lightness = {.points = {{0.0, 0.1}, {1.0, 0.9}}},
            },
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    expect(plan.segments.empty(), "neutral and disabled nodes compile to an empty plan");
    const auto result = image::execute_adjustment_nodes_with_backend(
        input,
        nodes,
        {},
        image::AdjustmentBackendMode::metal
    );
    expect(
        result.valid()
            && result.backend == image::AdjustmentBackend::cpu
            && !result.fell_back && result.diagnostic.empty()
            && result.pixels.samples == input.samples,
        "forced Metal records effective CPU/no-fallback when there is no executable work"
    );
}

void unsupported_operations_are_whole_stage_fallbacks() {
    const auto input = make_image(13U, 9U);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "gpu-prefix",
            .parameters = image::ExposureAdjustment{.stops = 0.5},
        },
        image::AdjustmentNode{
            .node_id = "unsupported-middle",
            .parameter_schema_version =
                image::detail_effects_parameter_schema_version,
            .implementation_version =
                image::finishing_effects_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass =
                    image::DetailEffectsExecutionPass::finishing_effects,
                .grain_amount = 0.35,
            },
        },
        image::AdjustmentNode{
            .node_id = "gpu-suffix",
            .parameters = image::SaturationAdjustment{.factor = 0.8},
        },
    };
    const auto cpu = image::execute_adjustment_nodes(input, nodes);
    const auto automatic = image::execute_adjustment_nodes_with_backend(
        input,
        nodes,
        {},
        image::AdjustmentBackendMode::automatic
    );
    expect(
        automatic.valid()
            && automatic.backend == image::AdjustmentBackend::cpu
            && automatic.fell_back && !automatic.diagnostic.empty()
            && automatic.pixels.samples == cpu.samples,
        "one unsupported middle node replays the complete immutable stage on CPU"
    );
    try {
        static_cast<void>(image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        ));
        expect(false, "forced Metal rejects a stage containing an unsupported active node");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::backend_failure,
            "forced unsupported Metal reports a typed backend failure"
        );
    }

    const std::array neighborhood_nodes{
        image::AdjustmentNode{
            .node_id = "active-neighborhood",
            .parameter_schema_version =
                image::selective_tone_parameter_schema_version,
            .implementation_version =
                image::selective_tone_implementation_version,
            .parameters = image::SelectiveToneAdjustment{.shadows = 0.25},
        },
    };
    const auto neighborhood_cpu =
        image::execute_adjustment_nodes(input, neighborhood_nodes);
    const auto neighborhood_automatic =
        image::execute_adjustment_nodes_with_backend(
            input,
            neighborhood_nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
    expect(
        neighborhood_automatic.backend == image::AdjustmentBackend::cpu
            && neighborhood_automatic.fell_back
            && neighborhood_automatic.pixels.samples == neighborhood_cpu.samples,
        "an active neighborhood segment forces complete-stage CPU replay"
    );
    try {
        static_cast<void>(image::execute_adjustment_nodes_with_backend(
            input,
            neighborhood_nodes,
            {},
            image::AdjustmentBackendMode::metal
        ));
        expect(false, "forced Metal rejects an active neighborhood segment");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::backend_failure,
            "forced neighborhood Metal rejection remains typed"
        );
    }
}

void color_warper_has_a_pixel_local_gpu_contract() {
    const auto input = make_image(37U, 23U);
    const std::array nodes{
        color_warper_node("connected-oklab-color-warp", color_warper_parameters()),
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto cpu = image::execute_adjustment_nodes(input, nodes);
    expect(
        plan.segments.size() == 1U
            && plan.segments.front().locality == image::AdjustmentLocality::pixel_local
            && plan.segments.front().steps
                == std::vector{image::EditExecutionStep{
                    .node_index = 0U,
                    .operation = image::AdjustmentOperation::oklab_color_warper,
                }}
            && cpu.samples != input.samples,
        "the Oklab Color Warper is an observable pixel-local lattice operation"
    );
    expect(
        image::operation_id(image::AdjustmentOperation::oklab_color_warper)
            == "shadow.oklab_color_warper",
        "the Oklab Color Warper exposes a stable operation identity"
    );

    const auto automatic = image::execute_adjustment_nodes_with_backend(
        input,
        nodes,
        {},
        image::AdjustmentBackendMode::automatic
    );
    if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        double automatic_error = 0.0;
        expect(
            automatic.backend == image::AdjustmentBackend::metal
                && !automatic.fell_back
                && close_to_cpu(automatic.pixels, cpu, automatic_error, 2.0e-4),
            "Color Warper automatic Metal execution matches the CPU lattice oracle"
        );
        const auto forced = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double forced_error = 0.0;
        expect(
            forced.backend == image::AdjustmentBackend::metal
                && !forced.fell_back
                && close_to_cpu(forced.pixels, cpu, forced_error, 2.0e-4),
            "forced Metal executes Color Warper within the CPU parity tolerance"
        );
    } else {
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back
                && automatic.pixels.samples == cpu.samples,
            "Color Warper remains a complete CPU replay when Metal is unavailable"
        );
    }

    const std::array neutral_nodes{
        color_warper_node("neutral-oklab-color-warp", image::OklabColorWarperAdjustment{}),
    };
    expect(
        image::compile_edit_execution_plan(neutral_nodes).segments.empty(),
        "the default Color Warper lattice is exactly neutral and elides from the plan"
    );
    auto zero_strength = color_warper_parameters();
    zero_strength.strength = 0.0;
    expect(
        image::compile_edit_execution_plan(std::array{
            color_warper_node("zero-strength-oklab-color-warp", std::move(zero_strength)),
        }).segments.empty(),
        "zero Color Warper strength elides an authored lattice without altering its geometry"
    );

    auto malformed = color_warper_parameters();
    malformed.control_points.front().a_offset = image::oklab_color_warper_maximum_offset + 0.001;
    try {
        static_cast<void>(image::compile_edit_execution_plan(std::array{
            color_warper_node("malformed-oklab-color-warp", std::move(malformed)),
        }));
        expect(false, "Color Warper rejects control-point moves beyond its declared guardrail");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::invalid_parameter,
            "out-of-range Color Warper controls report a typed parameter error"
        );
    }
}

void fp32_unsafe_curve_and_lut_domains_fall_back_before_dispatch() {
    const auto input = make_image(11U, 7U);
    const auto expect_lut_fallback = [&input](
        image::CubeLut3D lut,
        const std::string_view description
    ) {
        const std::array nodes{
            image::AdjustmentNode{
                .node_id = std::string(description),
                .parameters = image::CubeLutAdjustment{
                    .lut = std::move(lut),
                    .intensity = 1.0,
                },
            },
        };
        const auto cpu = image::execute_adjustment_nodes(input, nodes);
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back
                && automatic.diagnostic.find("domain") != std::string::npos
                && automatic.pixels.samples == cpu.samples,
            description
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal rejects an fp32-unsafe LUT domain");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "an fp32-unsafe LUT domain reports a typed backend failure"
            );
        }
    };

    auto collapsed_lut = advanced_test_lut();
    collapsed_lut.domain_min[0] = 1.0e20;
    collapsed_lut.domain_max[0] = std::nextafter(
        collapsed_lut.domain_min[0],
        std::numeric_limits<double>::infinity()
    );
    expect_lut_fallback(
        std::move(collapsed_lut),
        "a LUT interval that collapses during fp32 quantization replays on CPU"
    );

    auto overflowing_lut = advanced_test_lut();
    const double float_limit =
        static_cast<double>(std::numeric_limits<float>::max());
    overflowing_lut.domain_min[1] = -float_limit;
    overflowing_lut.domain_max[1] = float_limit;
    expect_lut_fallback(
        std::move(overflowing_lut),
        "a LUT interval whose fp32 span overflows replays on CPU"
    );

    const std::array curve_nodes{
        image::AdjustmentNode{
            .node_id = "fp32-collapsed-curve-knots",
            .parameter_schema_version =
                image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version =
                image::oklab_lightness_tone_curve_implementation_version,
            .parameters = image::OklabLightnessToneCurve{
                .lightness = {
                    .points = {
                        {0.0, 0.0},
                        {0.5, 0.42},
                        {
                            std::nextafter(
                                0.5,
                                std::numeric_limits<double>::infinity()
                            ),
                            0.58,
                        },
                        {1.0, 1.0},
                    },
                },
            },
        },
    };
    const auto curve_cpu = image::execute_adjustment_nodes(input, curve_nodes);
    const auto curve_automatic = image::execute_adjustment_nodes_with_backend(
        input,
        curve_nodes,
        {},
        image::AdjustmentBackendMode::automatic
    );
    expect(
        curve_automatic.backend == image::AdjustmentBackend::cpu
            && curve_automatic.fell_back
            && curve_automatic.diagnostic.find("curve knots")
                != std::string::npos
            && curve_automatic.pixels.samples == curve_cpu.samples,
        "curve knots that collapse during fp32 quantization replay on CPU"
    );
    try {
        static_cast<void>(image::execute_adjustment_nodes_with_backend(
            input,
            curve_nodes,
            {},
            image::AdjustmentBackendMode::metal
        ));
        expect(false, "forced Metal rejects fp32-collapsed curve knots");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::backend_failure,
            "fp32-collapsed curve knots report a typed backend failure"
        );
    }

    const auto expect_perceptual_fallback = [&input](
        image::PerceptualColorAdjustment parameters,
        const std::string_view expected_diagnostic,
        const std::string_view description
    ) {
        const std::array nodes{
            perceptual_node(std::string(description), std::move(parameters)),
        };
        const auto cpu = image::execute_adjustment_nodes(input, nodes);
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back
                && automatic.diagnostic.find(expected_diagnostic)
                    != std::string::npos
                && automatic.pixels.samples == cpu.samples,
            description
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal rejects fp32-collapsed perceptual controls");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "fp32-collapsed perceptual controls report a typed backend failure"
            );
        }
    };

    auto collapsed_point_range = primary_point_parameters();
    collapsed_point_range.color_range.softness =
        std::numeric_limits<double>::denorm_min();
    expect_perceptual_fallback(
        std::move(collapsed_point_range),
        "Point Color",
        "a nonzero Point Color parameter that collapses to zero in fp32 replays on CPU"
    );

    auto collapsed_selective_color = selective_color_parameters();
    collapsed_selective_color.selective_color_cmyk = {};
    collapsed_selective_color.selective_color_cmyk[0][0] =
        std::numeric_limits<double>::denorm_min();
    expect_perceptual_fallback(
        std::move(collapsed_selective_color),
        "Selective Color table",
        "a nonzero Selective Color amount that collapses to zero in fp32 replays on CPU"
    );
}

void opponent_balance_and_local_contrast_have_explicit_cpu_contract() {
    const auto input = make_image(71U, 43U);

    image::PerceptualColorAdjustment balance;
    balance.global_a_balance = 0.43;
    balance.global_b_balance = -0.31;
    const std::array balance_nodes{
        perceptual_node("global-oklab-opponent-balance", balance),
    };
    const auto balance_plan = image::compile_edit_execution_plan(balance_nodes);
    const auto balance_cpu = image::execute_adjustment_nodes(input, balance_nodes);
    expect(
        balance_plan.segments.size() == 1U
            && balance_plan.segments.front().locality == image::AdjustmentLocality::pixel_local
            && balance_cpu.samples != input.samples,
        "global Oklab a/b balance is a visible pixel-local adjustment"
    );
    const auto balance_automatic = image::execute_adjustment_nodes_with_backend(
        input,
        balance_nodes,
        {},
        image::AdjustmentBackendMode::automatic
    );
    if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        double automatic_error = 0.0;
        expect(
            balance_automatic.backend == image::AdjustmentBackend::metal
                && !balance_automatic.fell_back
                && close_to_cpu(
                    balance_automatic.pixels,
                    balance_cpu,
                    automatic_error,
                    2.0e-4
                ),
            "global Oklab balance has a pixel-local Metal path matching the CPU oracle"
        );
        const auto balance_metal = image::execute_adjustment_nodes_with_backend(
            input,
            balance_nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double forced_error = 0.0;
        expect(
            balance_metal.backend == image::AdjustmentBackend::metal
                && !balance_metal.fell_back
                && close_to_cpu(balance_metal.pixels, balance_cpu, forced_error, 2.0e-4),
            "forced Metal executes global Oklab balance within the CPU parity tolerance"
        );
    } else {
        expect(
            balance_automatic.backend == image::AdjustmentBackend::cpu
                && balance_automatic.fell_back
                && balance_automatic.pixels.samples == balance_cpu.samples,
            "global Oklab balance remains a complete CPU replay when Metal is unavailable"
        );
    }

    const image::OklabOpponentToneCurves opponent_curves{
        .a = {.points = {{0.0, -0.015}, {0.45, 0.064}, {1.0, 0.010}}},
        .b = {.points = {{0.0, 0.018}, {0.58, -0.052}, {1.0, 0.004}}},
    };
    const std::array opponent_nodes{
        image::AdjustmentNode{
            .node_id = "oklab-opponent-curves",
            .parameter_schema_version =
                image::oklab_opponent_tone_curve_parameter_schema_version,
            .implementation_version = image::oklab_opponent_tone_curve_implementation_version,
            .parameters = opponent_curves,
        },
    };
    const auto opponent_plan = image::compile_edit_execution_plan(opponent_nodes);
    const auto opponent_cpu = image::execute_adjustment_nodes(input, opponent_nodes);
    const auto opponent_automatic = image::execute_adjustment_nodes_with_backend(
        input,
        opponent_nodes,
        {},
        image::AdjustmentBackendMode::automatic
    );
    expect(
        opponent_plan.segments.size() == 1U
            && opponent_plan.segments.front().locality == image::AdjustmentLocality::pixel_local
            && opponent_cpu.samples != input.samples,
        "Oklab opponent curves are a visible pixel-local adjustment"
    );
    if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        double automatic_error = 0.0;
        expect(
            opponent_automatic.backend == image::AdjustmentBackend::metal
                && !opponent_automatic.fell_back
                && close_to_cpu(
                    opponent_automatic.pixels,
                    opponent_cpu,
                    automatic_error,
                    2.0e-4
                ),
            "Oklab opponent curves have a Metal path matching the CPU oracle"
        );
        const auto opponent_metal = image::execute_adjustment_nodes_with_backend(
            input,
            opponent_nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double forced_error = 0.0;
        expect(
            opponent_metal.backend == image::AdjustmentBackend::metal
                && !opponent_metal.fell_back
                && close_to_cpu(opponent_metal.pixels, opponent_cpu, forced_error, 2.0e-4),
            "forced Metal executes Oklab opponent curves within the CPU parity tolerance"
        );
    } else {
        expect(
            opponent_automatic.backend == image::AdjustmentBackend::cpu
                && opponent_automatic.fell_back
                && opponent_automatic.pixels.samples == opponent_cpu.samples,
            "Oklab opponent curves remain a complete CPU replay when Metal is unavailable"
        );
    }
    const std::array neutral_opponent_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-oklab-opponent-curves",
            .parameter_schema_version =
                image::oklab_opponent_tone_curve_parameter_schema_version,
            .implementation_version = image::oklab_opponent_tone_curve_implementation_version,
            .parameters = image::OklabOpponentToneCurves{},
        },
    };
    expect(
        image::compile_edit_execution_plan(neutral_opponent_nodes).segments.empty(),
        "the zero-valued Oklab opponent curve pair elides from the execution plan"
    );
    auto malformed_opponent = opponent_curves;
    malformed_opponent.a.points[1U].y = 0.121;
    try {
        static_cast<void>(image::compile_edit_execution_plan(std::array{
            image::AdjustmentNode{
                .node_id = "malformed-oklab-opponent-curves",
                .parameter_schema_version =
                    image::oklab_opponent_tone_curve_parameter_schema_version,
                .implementation_version = image::oklab_opponent_tone_curve_implementation_version,
                .parameters = std::move(malformed_opponent),
            },
        }));
        expect(false, "Oklab opponent curves reject offsets beyond their perceptual guardrail");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::invalid_parameter,
            "out-of-range Oklab opponent curves report a typed parameter error"
        );
    }

    const image::SharpenAdjustment local_contrast{
        .execution_pass = image::DetailEffectsExecutionPass::color_grading,
        .local_contrast = 0.62,
        .local_contrast_scale = 0.50,
    };
    const std::array detail_nodes{
        image::AdjustmentNode{
            .node_id = "edge-aware-local-contrast",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters = local_contrast,
        },
    };
    const auto detail_plan = image::compile_edit_execution_plan(detail_nodes);
    const auto detail_cpu = image::execute_adjustment_nodes(input, detail_nodes);
    const auto detail_footprint = image::footprint(local_contrast);
    expect(
        detail_plan.segments.size() == 1U
            && detail_plan.segments.front().locality == image::AdjustmentLocality::neighborhood
            && detail_footprint.horizontal_radius >= 100U
            && detail_footprint.vertical_radius >= 100U
            && detail_cpu.samples != input.samples,
        "Local Contrast declares its broad guided-filter neighborhood footprint"
    );
    const auto detail_automatic = image::execute_adjustment_nodes_with_backend(
        input,
        detail_nodes,
        {},
        image::AdjustmentBackendMode::automatic
    );
    expect(
        detail_automatic.backend == image::AdjustmentBackend::cpu
            && detail_automatic.fell_back
            && detail_automatic.diagnostic.find("pixel-local")
                != std::string::npos
            && detail_automatic.pixels.samples == detail_cpu.samples,
        "Local Contrast requests a complete CPU replay instead of a partial Metal result"
    );
}

void malformed_disabled_nodes_fail_before_backend_selection() {
    const auto input = make_image(5U, 3U);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "malformed-disabled",
            .enabled = false,
            .parameters = image::ExposureAdjustment{
                .stops = std::numeric_limits<double>::quiet_NaN(),
            },
        },
    };
    for (const auto mode : std::array{
             image::AdjustmentBackendMode::cpu,
             image::AdjustmentBackendMode::automatic,
             image::AdjustmentBackendMode::metal,
         }) {
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                mode
            ));
            expect(false, "a malformed disabled node cannot bypass dispatcher validation");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::invalid_parameter
                    && error.node_index() == 0U,
                "all backend modes preserve malformed disabled-node provenance"
            );
        }
    }
}

void backend_availability_and_resource_failure_are_explicit() {
    const auto input = make_image(17U, 11U);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "active-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.25},
        },
    };
    const auto cpu = image::execute_adjustment_nodes(input, nodes);
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        const bool metal_required =
            std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr;
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back && automatic.pixels.samples == cpu.samples,
            "CPU-only automatic selection replays the complete stage"
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal fails when its runtime is unavailable");
        } catch (const image::EditError& error) {
            if (metal_required) {
                std::cerr << "Metal adjustment diagnostic: " << error.what() << '\n';
            }
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "unavailable forced Metal returns a typed backend failure"
            );
        }
        expect(!metal_required, "Metal was required but its adjustment backend is unavailable");
        return;
    }

    {
        const ScopedEnvironment tiny_budget(
            "SHADOW_TEST_METAL_ADJUSTMENT_TILE_BYTES",
            "1"
        );
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back && automatic.pixels.samples == cpu.samples,
            "resource rejection replays the complete adjustment stage on CPU"
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal exposes resource rejection");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "forced resource rejection remains a backend failure"
            );
        }
    }
    {
        // 17 RGB float pixels require 204 bytes per row and 408 bytes for separate input/output.
        // A 900-byte budget admits two rows, forcing this 11-row image through six GPU tiles.
        const ScopedEnvironment multi_tile_budget(
            "SHADOW_TEST_METAL_ADJUSTMENT_TILE_BYTES",
            "900"
        );
        const auto metal = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double error = 0.0;
        expect(
            metal.backend == image::AdjustmentBackend::metal
                && !metal.fell_back
                && close_to_cpu(metal.pixels, cpu, error),
            "successful bounded multi-tile Metal execution matches CPU"
        );
    }
    {
        const ScopedEnvironment injected(
            "SHADOW_TEST_METAL_ADJUSTMENT_FORCE_FAILURE",
            "1"
        );
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back && automatic.pixels.samples == cpu.samples,
            "runtime GPU failure discards all GPU output and replays CPU"
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal exposes an injected runtime failure");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "injected forced-Metal failure remains typed"
            );
        }
    }
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
            std::cerr << "Advanced standalone parity operation=" << index
                      << " max=" << error << '\n';
        }
        expect(
            metal.backend == image::AdjustmentBackend::metal
                && !metal.fell_back && parity,
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
    const bool combined_parity =
        close_to_cpu(metal.pixels, cpu, combined_error, 8.0e-5);
    if (!combined_parity) {
        std::cerr << "Advanced standalone combined parity max="
                  << combined_error << '\n';
    }
    expect(
        metal.backend == image::AdjustmentBackend::metal
            && !metal.fell_back && combined_parity,
        "Oklab curve, pure color grading, and LUT preserve CPU order and semantics on Metal"
    );
    worst_error = std::max(worst_error, combined_error);

    std::vector<image::ToneCurvePoint> dense_points;
    dense_points.reserve(image::maximum_tone_curve_points);
    for (std::size_t index = 0U;
         index < image::maximum_tone_curve_points;
         ++index) {
        const double x = static_cast<double>(index)
            / static_cast<double>(image::maximum_tone_curve_points - 1U);
        dense_points.push_back({
            .x = x,
            .y = x + 0.04 * x * (1.0 - x),
        });
    }
    const std::array dense_curve{
        image::AdjustmentNode{
            .node_id = "maximum-density-oklab-curve",
            .parameter_schema_version =
                image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version =
                image::oklab_lightness_tone_curve_implementation_version,
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
        dense_metal.backend == image::AdjustmentBackend::metal
            && !dense_metal.fell_back
            && close_to_cpu(
                dense_metal.pixels,
                dense_cpu,
                dense_error,
                8.0e-5
            ),
        "a maximum-density 256-point curve remains eligible and CPU-equivalent on Metal"
    );
    worst_error = std::max(worst_error, dense_error);
    std::cout << "Metal advanced-operation maximum absolute error: "
              << worst_error << '\n';
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
        const bool linear_parity = close_to_cpu(
            metal.pixels,
            cpu.pixels,
            linear_error,
            2.0e-4
        );
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_cpu_reference(
                cpu.pixels,
                {.target_dimensions = input.dimensions}
            );
        const auto metal_display =
            image::render_linear_srgb_to_display_srgb8_cpu_reference(
                metal.pixels,
                {.target_dimensions = input.dimensions}
            );
        const std::uint8_t display_error = maximum_rgb8_difference(
            metal_display.bytes,
            cpu_display.bytes
        );
        if (!linear_parity || display_error > 1U) {
            std::cerr << "Perceptual Metal parity " << description
                      << ": linear max=" << linear_error
                      << ", display max=" << static_cast<unsigned int>(display_error)
                      << '\n';
        }
        expect(
            metal.backend == image::AdjustmentBackend::metal
                && !metal.fell_back && linear_parity,
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

    const auto advanced = advanced_nodes();
    const std::array combined{
        image::AdjustmentNode{
            .node_id = "pre-perceptual-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.42},
        },
        perceptual_node(
            "combined-perceptual-between-nodes",
            combined_perceptual_parameters()
        ),
        advanced[0],
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
    const auto perceptual = perceptual_node(
        "order-perceptual",
        combined_perceptual_parameters()
    );
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

    std::cout << "Metal PerceptualColor maximum absolute error: "
              << worst_linear_error << "; display RGB8 max="
              << static_cast<unsigned int>(worst_display_error) << '\n';
}

void every_core_order_matches_the_cpu_oracle() {
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        return;
    }
    const auto input = make_image(37U, 23U, true);
    const auto source = core_nodes();
    std::array<std::size_t, 4U> order{0U, 1U, 2U, 3U};
    std::size_t permutation_count = 0U;
    double worst_error = 0.0;
    do {
        std::array<image::AdjustmentNode, 4U> nodes{
            source[order[0]],
            source[order[1]],
            source[order[2]],
            source[order[3]],
        };
        const auto cpu = image::execute_adjustment_nodes(input, nodes);
        const auto metal = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double error = 0.0;
        expect(
            metal.valid()
                && metal.backend == image::AdjustmentBackend::metal
                && !metal.fell_back
                && close_to_cpu(metal.pixels, cpu, error),
            "Metal preserves one of the 24 core-operation orders"
        );
        worst_error = std::max(worst_error, error);
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));
    expect(permutation_count == 24U, "all 24 core-operation permutations execute");
    std::cout << "Metal adjustment 24-order maximum absolute error: "
              << worst_error << '\n';
}

void randomized_and_endpoint_parameters_match_the_cpu_oracle() {
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        return;
    }
    const auto input = make_image(29U, 17U, true);
    std::mt19937 generator(0x7139U);
    std::uniform_real_distribution<double> white_balance(-1.0, 1.0);
    std::uniform_real_distribution<double> exposure(-2.5, 2.5);
    std::uniform_real_distribution<double> contrast(0.05, 4.0);
    std::uniform_real_distribution<double> saturation(0.0, 2.5);
    double worst_error = 0.0;
    std::vector<unsigned int> display_code_errors;
    display_code_errors.reserve(
        static_cast<std::size_t>(64U * input.dimensions.pixel_count() * 3U)
    );
    for (std::size_t iteration = 0U; iteration < 64U; ++iteration) {
        std::array nodes{
            image::AdjustmentNode{
                .node_id = "random-wb",
                .parameters = image::RgbWhiteBalanceAdjustment{
                    .temperature = iteration == 0U ? 1.0 : white_balance(generator),
                    .tint = iteration == 1U ? -1.0 : white_balance(generator),
                },
            },
            image::AdjustmentNode{
                .node_id = "random-exposure",
                .parameters = image::ExposureAdjustment{
                    .stops = exposure(generator),
                },
            },
            image::AdjustmentNode{
                .node_id = "random-contrast",
                .parameters = image::ContrastAdjustment{
                    .factor = iteration == 0U ? 0.0 : contrast(generator),
                    .pivot = iteration == 1U ? 0.0 : 0.18,
                },
            },
            image::AdjustmentNode{
                .node_id = "random-saturation",
                .parameters = image::SaturationAdjustment{
                    .factor = iteration == 0U ? 0.0 : saturation(generator),
                },
            },
        };
        std::shuffle(nodes.begin(), nodes.end(), generator);
        const auto cpu = image::execute_adjustment_nodes(input, nodes);
        const auto metal = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double error = 0.0;
        // The strict normal-edit contract is exercised above. These randomized endpoint cases
        // intentionally combine maximum tint, tiny contrast pivots and >2x saturation, where
        // consecutive Oklab round trips accumulate fp32 error. Keep a coarse linear guard, then
        // enforce the user-visible contract through the exact same CPU display oracle.
        const bool parity = close_to_cpu(metal.pixels, cpu, error, 1.0e-3);
        if (!parity) {
            std::cerr << "Random parity iteration " << iteration << " order/parameters:";
            for (const auto& node : nodes) {
                std::cerr << ' ' << image::operation_id(image::operation(node.parameters));
                if (const auto* value =
                        std::get_if<image::RgbWhiteBalanceAdjustment>(&node.parameters)) {
                    std::cerr << '(' << value->temperature << ',' << value->tint << ')';
                } else if (const auto* value =
                               std::get_if<image::ExposureAdjustment>(&node.parameters)) {
                    std::cerr << '(' << value->stops << ')';
                } else if (const auto* value =
                               std::get_if<image::ContrastAdjustment>(&node.parameters)) {
                    std::cerr << '(' << value->factor << ',' << value->pivot << ')';
                } else if (const auto* value =
                               std::get_if<image::SaturationAdjustment>(&node.parameters)) {
                    std::cerr << '(' << value->factor << ')';
                }
            }
            std::cerr << '\n';
        }
        expect(
            metal.backend == image::AdjustmentBackend::metal
                && parity,
            "random, endpoint, factor-zero and super-white parameters match CPU"
        );
        worst_error = std::max(worst_error, error);
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_cpu_reference(
                cpu,
                image::DisplayOutputRequest{.target_dimensions = cpu.dimensions}
            );
        const auto metal_display =
            image::render_linear_srgb_to_display_srgb8_cpu_reference(
                metal.pixels,
                image::DisplayOutputRequest{
                    .target_dimensions = metal.pixels.dimensions,
                }
            );
        for (std::size_t index = 0U; index < cpu_display.bytes.size(); ++index) {
            display_code_errors.push_back(static_cast<unsigned int>(std::abs(
                static_cast<int>(cpu_display.bytes[index])
                - static_cast<int>(metal_display.bytes[index])
            )));
        }
    }
    std::sort(display_code_errors.begin(), display_code_errors.end());
    const unsigned int maximum_code_error = display_code_errors.back();
    const std::size_t p99_index = static_cast<std::size_t>(
        static_cast<double>(display_code_errors.size() - 1U) * 0.99
    );
    const unsigned int p99_code_error = display_code_errors[p99_index];
    const double mean_code_error =
        static_cast<double>(std::accumulate(
            display_code_errors.begin(),
            display_code_errors.end(),
            std::uint64_t{0U}
        )) / static_cast<double>(display_code_errors.size());
    expect(
        maximum_code_error <= 1U && p99_code_error <= 1U && mean_code_error <= 0.02,
        "extreme fp32 adjustment differences stay below one display code"
    );
    std::cout << "Metal adjustment randomized maximum absolute error: "
              << worst_error << "; display RGB8 max=" << maximum_code_error
              << ", p99=" << p99_code_error << ", mean=" << mean_code_error << '\n';
}

void repeated_nodes_and_concurrent_renders_are_deterministic() {
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        return;
    }
    const auto input = make_image(79U, 53U);
    const auto core = core_nodes();
    std::vector<image::AdjustmentNode> repeated;
    repeated.reserve(12U);
    for (std::size_t repetition = 0U; repetition < 3U; ++repetition) {
        repeated.insert(repeated.end(), core.begin(), core.end());
    }
    const auto cpu = image::execute_adjustment_nodes(input, repeated);
    auto render = [&]() {
        return image::execute_adjustment_nodes_with_backend(
            input,
            repeated,
            {},
            image::AdjustmentBackendMode::metal
        );
    };
    auto first = std::async(std::launch::async, render);
    auto second = std::async(std::launch::async, render);
    const auto first_result = first.get();
    const auto second_result = second.get();
    double error = 0.0;
    expect(
        first_result.backend == image::AdjustmentBackend::metal
            && second_result.backend == image::AdjustmentBackend::metal
            && first_result.pixels.samples == second_result.pixels.samples
            && close_to_cpu(first_result.pixels, cpu, error),
        "repeated-node concurrent Metal renders are deterministic and match CPU"
    );
}

void optional_true_machine_benchmark() {
    const char* enabled = std::getenv("SHADOW_TEST_ADJUSTMENT_BENCHMARK");
    if (enabled == nullptr || std::string_view(enabled) != "1"
        || !image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        return;
    }
    const auto nodes = core_nodes();
    for (const auto dimensions : std::array{
             image::Dimensions{1200U, 800U},
             image::Dimensions{2048U, 1365U},
         }) {
        const auto input = make_image(dimensions.width, dimensions.height);
        // Pay source compilation and pipeline creation before the warm measurement.
        static_cast<void>(image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        ));
        const auto measure = [&](const image::AdjustmentBackendMode backend) {
            constexpr std::size_t iterations = 20U;
            std::array<double, iterations> milliseconds{};
            for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
                const auto start = std::chrono::steady_clock::now();
                static_cast<void>(image::execute_adjustment_nodes_with_backend(
                    input,
                    nodes,
                    {},
                    backend
                ));
                const auto end = std::chrono::steady_clock::now();
                milliseconds[iteration] =
                    std::chrono::duration<double, std::milli>(end - start).count();
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            return std::pair{
                milliseconds[iterations / 2U],
                milliseconds[18U],
            };
        };
        const auto [cpu_p50, cpu_p95] = measure(image::AdjustmentBackendMode::cpu);
        const auto [metal_p50, metal_p95] = measure(image::AdjustmentBackendMode::metal);
        std::cout << "Adjustment benchmark " << dimensions.width << 'x'
                  << dimensions.height << ": CPU p50=" << cpu_p50
                  << " ms/p95=" << cpu_p95 << " ms, Metal p50=" << metal_p50
                  << " ms/p95=" << metal_p95
                  << " ms, p50 speedup=" << cpu_p50 / metal_p50 << "x\n";
    }
}

} // namespace

int main() {
    neutral_and_disabled_plans_have_no_backend_route();
    unsupported_operations_are_whole_stage_fallbacks();
    color_warper_has_a_pixel_local_gpu_contract();
    fp32_unsafe_curve_and_lut_domains_fall_back_before_dispatch();
    opponent_balance_and_local_contrast_have_explicit_cpu_contract();
    malformed_disabled_nodes_fail_before_backend_selection();
    backend_availability_and_resource_failure_are_explicit();
    advanced_pixel_local_operations_match_the_cpu_oracle();
    perceptual_color_matches_cpu_and_display_oracles_on_metal();
    every_core_order_matches_the_cpu_oracle();
    randomized_and_endpoint_parameters_match_the_cpu_oracle();
    repeated_nodes_and_concurrent_renders_are_deterministic();
    optional_true_machine_benchmark();
    if (failures != 0) {
        std::cerr << failures << " adjustment execution contract checks failed\n";
        return 1;
    }
    std::cout << "adjustment execution contract checks passed\n";
    return 0;
}

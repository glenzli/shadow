#include "adjustment_execution_contract_cases.hpp"
#include "advanced_operation_fixture.hpp"
#include "execution_parity_fixture.hpp"
#include "perceptual_operation_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract {

namespace {

using advanced_fixture::advanced_test_lut;
using parity_fixture::make_image;
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
} // namespace

int run_neutral_and_disabled_plans_have_no_backend_route() {
    failures = 0;
    neutral_and_disabled_plans_have_no_backend_route();
    return failures;
}

int run_unsupported_operations_are_whole_stage_fallbacks() {
    failures = 0;
    unsupported_operations_are_whole_stage_fallbacks();
    return failures;
}

int run_fp32_unsafe_curve_and_lut_domains_fall_back_before_dispatch() {
    failures = 0;
    fp32_unsafe_curve_and_lut_domains_fall_back_before_dispatch();
    return failures;
}

int run_malformed_disabled_nodes_fail_before_backend_selection() {
    failures = 0;
    malformed_disabled_nodes_fail_before_backend_selection();
    return failures;
}

} // namespace shadow::image::adjustment_execution_contract

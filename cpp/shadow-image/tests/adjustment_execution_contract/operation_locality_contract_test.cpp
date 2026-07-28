#include "adjustment_execution_contract_cases.hpp"
#include "execution_parity_fixture.hpp"
#include "perceptual_operation_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <array>
#include <iostream>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract {

namespace {

using parity_fixture::close_to_cpu;
using parity_fixture::make_image;
using perceptual_fixture::perceptual_node;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
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
} // namespace

int run_opponent_balance_and_local_contrast_have_explicit_cpu_contract() {
    failures = 0;
    opponent_balance_and_local_contrast_have_explicit_cpu_contract();
    return failures;
}

} // namespace shadow::image::adjustment_execution_contract

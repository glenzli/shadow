#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string_view>

namespace image = shadow::image;

namespace shadow::image::warm_edit_gpu_contract {

namespace {

using parity_fixture::linear_close;
using parity_fixture::make_random_image;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void resident_gpu_texture_is_complete_or_declines() {
    const auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU Texture was required but no resident Metal session could be prepared"
        );
        return;
    }

    std::array<image::AdjustmentNode, 3U> nodes{
        image::AdjustmentNode{
            .node_id = "before-texture-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.18},
        },
        image::AdjustmentNode{
            .node_id = "perceptual-texture",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                    .texture = 0.72,
                    .shadows_hue = 24.0,
                    .shadows_saturation = 0.17,
                    .midtones_hue = 148.0,
                    .midtones_saturation = 0.12,
                    .highlights_hue = 248.0,
                    .highlights_saturation = 0.21,
                    .grading_blending = 0.68,
                    .grading_balance = -0.18,
                },
        },
        image::AdjustmentNode{
            .node_id = "after-texture-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.83},
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu = preparation.session->render(nodes, plan, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a supported Oklab-L Texture stage completes on the resident GPU"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity =
            linear_close(*gpu.output->analyzed_linear, cpu.pixels, maximum_error, 2.5e-4);
        if (!linear_parity) {
            std::cerr << "Texture warm linear parity max=" << maximum_error << '\n';
        }
        expect(linear_parity, "the resident Texture path tracks the CPU Oklab-L reference");
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).clarity = 0.25;
    const auto combined_plan = image::compile_edit_execution_plan(nodes);
    const auto combined = preparation.session->render(nodes, combined_plan, true);
    expect(
        combined.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && combined.output.has_value() && combined.output->analyzed_linear.has_value(),
        "full-resolution Texture plus Clarity completes as one resident GPU stage"
    );
    if (combined.output && combined.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity =
            linear_close(*combined.output->analyzed_linear, cpu.pixels, maximum_error, 3.5e-4);
        if (!linear_parity) {
            std::cerr << "Full-resolution Texture + Clarity warm linear parity max="
                      << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "full-resolution resident Texture plus Clarity tracks the CPU reference"
        );
    }
}

void resident_gpu_clarity_is_complete_or_declines() {
    auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU Clarity was required but no resident Metal session could be prepared"
        );
        return;
    }

    std::array<image::AdjustmentNode, 3U> nodes{
        image::AdjustmentNode{
            .node_id = "before-clarity-exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.12},
        },
        image::AdjustmentNode{
            .node_id = "perceptual-clarity",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                    .clarity = 0.64,
                    .shadows_hue = 38.0,
                    .shadows_saturation = 0.14,
                    .midtones_hue = 154.0,
                    .midtones_saturation = 0.10,
                    .highlights_hue = 236.0,
                    .highlights_saturation = 0.19,
                    .grading_blending = 0.64,
                    .grading_balance = 0.15,
                },
        },
        image::AdjustmentNode{
            .node_id = "after-clarity-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.08},
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu = preparation.session->render(nodes, plan, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a full-resolution Oklab-L Clarity stage completes on the resident GPU"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity =
            linear_close(*gpu.output->analyzed_linear, cpu.pixels, maximum_error, 3.0e-4);
        if (!linear_parity) {
            std::cerr << "Clarity warm linear parity max=" << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the full-resolution resident Clarity path tracks the CPU Oklab-L reference"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).texture = 0.25;
    const auto combined_plan = image::compile_edit_execution_plan(nodes);
    const auto combined = preparation.session->render(nodes, combined_plan, true);
    expect(
        combined.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && combined.output.has_value() && combined.output->analyzed_linear.has_value(),
        "a full-resolution Texture plus Clarity stage completes on the resident GPU"
    );
    if (combined.output && combined.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity =
            linear_close(*combined.output->analyzed_linear, cpu.pixels, maximum_error, 3.5e-4);
        if (!linear_parity) {
            std::cerr << "Texture + Clarity warm linear parity max=" << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the combined resident Texture plus Clarity path tracks the CPU reference"
        );
    }
}

void resident_gpu_local_contrast_is_complete_or_declines() {
    auto source = make_random_image(193U, 113U, true);
    // The broad guided support is admitted only at preview scale. A full-size
    // image retains the complete CPU oracle rather than silently shrinking the
    // requested photographic radius.
    source.level_zero_to_raster_scale_x = 0.25;
    source.level_zero_to_raster_scale_y = 0.25;
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU Local Contrast was required but no resident Metal session could be prepared"
        );
        return;
    }

    std::array<image::AdjustmentNode, 3U> nodes{
        image::AdjustmentNode{
            .node_id = "before-local-contrast-exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.12},
        },
        image::AdjustmentNode{
            .node_id = "edge-aware-local-contrast",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                    .local_contrast = 0.62,
                    .local_contrast_scale = 0.50,
                    .shadows_hue = 38.0,
                    .shadows_saturation = 0.14,
                    .midtones_hue = 154.0,
                    .midtones_saturation = 0.10,
                    .highlights_hue = 236.0,
                    .highlights_saturation = 0.19,
                    .grading_blending = 0.64,
                    .grading_balance = 0.15,
                },
        },
        image::AdjustmentNode{
            .node_id = "after-local-contrast-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.08},
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu = preparation.session->render(nodes, plan, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a preview-scale guided Local Contrast stage completes on the resident GPU"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity =
            linear_close(*gpu.output->analyzed_linear, cpu.pixels, maximum_error, 5.0e-4);
        if (!linear_parity) {
            std::cerr << "Local Contrast warm linear parity max=" << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the resident guided Local Contrast path tracks the CPU Oklab-L reference"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).clarity = 0.25;
    const auto unsupported_plan = image::compile_edit_execution_plan(nodes);
    const auto unsupported = preparation.session->render(nodes, unsupported_plan, false);
    expect(
        unsupported.status == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
            && !unsupported.output.has_value() && !unsupported.diagnostic.empty(),
        "Local Contrast plus another neighbourhood band declines as one coherent CPU fallback"
    );
}

} // namespace

int run_resident_gpu_texture_contract() {
    failures = 0;
    resident_gpu_texture_is_complete_or_declines();
    return failures;
}

int run_resident_gpu_clarity_contract() {
    failures = 0;
    resident_gpu_clarity_is_complete_or_declines();
    return failures;
}

int run_resident_gpu_local_contrast_contract() {
    failures = 0;
    resident_gpu_local_contrast_is_complete_or_declines();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"
#include "../../src/proxy/warm_edit_gpu_render_plan.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>

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

using ComposedNodes = std::array<image::AdjustmentNode, 7U>;

[[nodiscard]] ComposedNodes composed_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "foundation-exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.17},
        },
        image::AdjustmentNode{
            .node_id = "guided-selective-tone",
            .parameter_schema_version = image::selective_tone_parameter_schema_version,
            .implementation_version = image::selective_tone_implementation_version,
            .parameters =
                image::SelectiveToneAdjustment{
                    .highlights = -0.42,
                    .shadows = 0.56,
                    .whites = -0.18,
                    .blacks = 0.22,
                },
        },
        image::AdjustmentNode{
            .node_id = "between-stage-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.93},
        },
        image::AdjustmentNode{
            .node_id = "technical-sharpen",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::technical_detail,
                    .amount = 0.28,
                    .radius = 1.2,
                    .threshold = 0.06,
                    .masking = 0.25,
                },
        },
        image::AdjustmentNode{
            .node_id = "between-stage-white-balance",
            .parameters =
                image::RgbWhiteBalanceAdjustment{
                    .temperature = 0.08,
                    .tint = -0.04,
                },
        },
        image::AdjustmentNode{
            .node_id = "creative-detail",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::color_grading_implementation_version,
            .parameters =
                image::SharpenAdjustment{
                    .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                    .clarity = 0.31,
                    .texture = 0.36,
                    .local_contrast = 0.27,
                    .local_contrast_scale = 0.72,
                    .shadows_hue = 28.0,
                    .shadows_saturation = 0.09,
                    .midtones_hue = 142.0,
                    .midtones_saturation = 0.07,
                    .highlights_hue = 238.0,
                    .highlights_saturation = 0.11,
                    .grading_blending = 0.62,
                    .grading_balance = -0.12,
                },
        },
        image::AdjustmentNode{
            .node_id = "final-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.04},
        },
    };
}

void composed_neighbourhood_plan_preserves_order_and_parity() {
    auto source = make_random_image(193U, 113U, true);
    const auto nodes = composed_nodes();
    const auto execution = image::compile_edit_execution_plan(
        nodes,
        source.level_zero_to_raster_scale_x,
        source.level_zero_to_raster_scale_y
    );
    const auto render_plan = image::detail::prepare_warm_gpu_render_plan(
        nodes,
        execution,
        source.dimensions,
        source.working_space,
        source.level_zero_to_raster_scale_x,
        source.level_zero_to_raster_scale_y,
        {}
    );
    expect(
        render_plan.complete && render_plan.passes.size() == 3U
            && render_plan.passes[0U].before.segments.size() == 1U
            && render_plan.passes[1U].before.segments.size() == 1U
            && render_plan.passes[2U].before.segments.size() == 1U
            && render_plan.after.segments.size() == 1U,
        "the portable render plan retains three neighbourhood stages and every pixel-local gap"
    );

    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "composed GPU stages were required but no resident Metal session could be prepared"
        );
        return;
    }
    const auto gpu = preparation.session->render(nodes, execution, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "three full-resolution neighbourhood stages complete in one resident Metal command"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool parity =
            linear_close(*gpu.output->analyzed_linear, cpu.pixels, maximum_error, 2.2e-3);
        if (!parity) {
            std::cerr << "Composed warm-stage parity max=" << maximum_error << '\n';
        }
        expect(
            parity,
            "composed Selective Tone, technical detail, creative detail and color gaps track CPU"
        );
    }
}

template <typename Callable>
[[nodiscard]] double median_milliseconds(const std::size_t iterations, Callable&& callable) {
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        callable();
        const auto finished = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(finished - started).count());
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2U];
}

void benchmark_composed_neighbourhood_plan_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_COMPOSED_BENCHMARK") == nullptr) {
        return;
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    auto source = make_random_image(dimensions.width, dimensions.height, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        std::cerr << "BENCH composed warm stages unavailable: " << preparation.diagnostic << '\n';
        ++failures;
        return;
    }
    const auto nodes = composed_nodes();
    const auto plan = image::compile_edit_execution_plan(
        nodes,
        source.level_zero_to_raster_scale_x,
        source.level_zero_to_raster_scale_y
    );
    std::uint64_t checksum = 0U;
    const double first_resident = median_milliseconds(1U, [&]() {
        auto rendered = preparation.session->render(nodes, plan, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    static_cast<void>(image::execute_adjustment_nodes_with_backend(
        source,
        nodes,
        {.full_dimensions = dimensions},
        image::AdjustmentBackendMode::cpu
    ));
    constexpr std::size_t iterations = 3U;
    const double cpu = median_milliseconds(iterations, [&]() {
        auto adjusted = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = dimensions},
            image::AdjustmentBackendMode::cpu
        );
        auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
            adjusted.pixels,
            {.target_dimensions = dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        checksum += displayed.bytes[displayed.bytes.size() / 2U];
    });
    const double resident = median_milliseconds(iterations, [&]() {
        auto rendered = preparation.session->render(nodes, plan, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH composed warm stages "
              << dimensions.width << 'x' << dimensions.height << " CPU-adjust+display=" << cpu
              << "ms"
              << " first-resident-Metal=" << first_resident << "ms"
              << " resident-Metal=" << resident << "ms"
              << " speedup=" << cpu / resident << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int run_resident_gpu_composed_stage_contract() {
    failures = 0;
    composed_neighbourhood_plan_preserves_order_and_parity();
    benchmark_composed_neighbourhood_plan_when_requested();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

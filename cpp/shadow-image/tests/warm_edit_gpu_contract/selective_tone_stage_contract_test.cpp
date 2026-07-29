#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"

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

using SelectiveToneNodes = std::array<image::AdjustmentNode, 3U>;

[[nodiscard]] SelectiveToneNodes selective_tone_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "before-selective-tone-exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.14},
        },
        image::AdjustmentNode{
            .node_id = "guided-selective-tone",
            .parameter_schema_version = image::selective_tone_parameter_schema_version,
            .implementation_version = image::selective_tone_implementation_version,
            .parameters =
                image::SelectiveToneAdjustment{
                    .highlights = -0.58,
                    .shadows = 0.71,
                    .whites = -0.23,
                    .blacks = 0.31,
                },
        },
        image::AdjustmentNode{
            .node_id = "after-selective-tone-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.91},
        },
    };
}

void resident_gpu_selective_tone_is_complete_or_declines() {
    const auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU Selective Tone was required but no resident Metal session could be prepared"
        );
        return;
    }

    auto nodes = selective_tone_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu = preparation.session->render(nodes, plan, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a full-resolution guided Selective Tone stage completes on the resident GPU"
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
            linear_close(*gpu.output->analyzed_linear, cpu.pixels, maximum_error, 1.5e-3);
        if (!linear_parity) {
            std::cerr << "Selective Tone warm linear parity max=" << maximum_error << '\n';
        }
        expect(linear_parity, "the resident guided Selective Tone path tracks the CPU reference");
    }

    std::vector<image::AdjustmentNode> composed(nodes.begin(), nodes.end());
    composed.push_back(
        image::AdjustmentNode{
            .node_id = "second-neighbourhood-stage",
            .parameters = image::SharpenAdjustment{
                .amount = 0.35,
                .radius = 1.1,
                .threshold = 0.04,
            },
        }
    );
    const auto composed_plan = image::compile_edit_execution_plan(composed);
    const auto composed_render = preparation.session->render(composed, composed_plan, true);
    expect(
        composed_render.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && composed_render.output.has_value()
            && composed_render.output->analyzed_linear.has_value(),
        "Selective Tone and technical detail remain in one ordered resident GPU transaction"
    );
    if (composed_render.output && composed_render.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            composed,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *composed_render.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            1.8e-3
        );
        if (!linear_parity) {
            std::cerr << "Selective Tone + technical detail parity max=" << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "composed neighbourhood stages preserve CPU operation order and numerical parity"
        );
    }

    composed.push_back(
        image::AdjustmentNode{
            .node_id = "unsupported-retouch-stage",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 0.5,
                    .center_y = 0.5,
                    .radius_level_zero_pixels = 2U,
                }},
            },
        }
    );
    const auto unsupported_plan = image::compile_edit_execution_plan(composed);
    const auto unsupported = preparation.session->render(composed, unsupported_plan, false);
    expect(
        unsupported.status == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
            && !unsupported.output.has_value() && !unsupported.diagnostic.empty(),
        "one unsupported neighbourhood stage still declines the complete GPU transaction"
    );
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

void benchmark_selective_tone_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_SELECTIVE_TONE_BENCHMARK") == nullptr) {
        return;
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto source = make_random_image(dimensions.width, dimensions.height, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        std::cerr << "BENCH Selective Tone unavailable: " << preparation.diagnostic << '\n';
        ++failures;
        return;
    }
    const auto nodes = selective_tone_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
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
    constexpr std::size_t iterations = 5U;
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
    std::cout << std::fixed << std::setprecision(3) << "BENCH Selective Tone " << dimensions.width
              << 'x' << dimensions.height << " CPU-adjust+display=" << cpu << "ms"
              << " first-resident-Metal=" << first_resident << "ms"
              << " resident-Metal=" << resident << "ms"
              << " speedup=" << cpu / resident << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int run_resident_gpu_selective_tone_contract() {
    failures = 0;
    resident_gpu_selective_tone_is_complete_or_declines();
    benchmark_selective_tone_when_requested();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

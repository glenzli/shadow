#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"

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

[[nodiscard]] std::vector<image::AdjustmentNode> clone_nodes(const std::size_t point_count = 4U) {
    image::RetouchStroke stroke{
        .radius_level_zero_pixels = 7U,
        .mode = image::SpotRepairMode::clone,
        .source_offset_x_radii = 1.65,
        .source_offset_y_radii = -0.75,
        .feather = 0.31,
        .strength = 0.41,
    };
    stroke.points.reserve(point_count);
    for (std::size_t index = 0U; index < point_count; ++index) {
        const double progress =
            point_count == 1U ? 0.0
                              : static_cast<double>(index) / static_cast<double>(point_count - 1U);
        stroke.points.push_back({
            .x = 0.18 + 0.58 * progress,
            .y = 0.28 + 0.22 * progress + 0.04 * std::sin(progress * 9.0),
        });
    }
    return {
        image::AdjustmentNode{
            .node_id = "before-clone",
            .parameters = image::ExposureAdjustment{.stops = 0.12},
        },
        image::AdjustmentNode{
            .node_id = "continuous-clone",
            .parameters =
                image::SpotHealAdjustment{
                    .spots = {{
                        .center_x = 0.78,
                        .center_y = 0.68,
                        .radius_level_zero_pixels = 5U,
                        .mode = image::SpotRepairMode::clone,
                        .source_offset_x_radii = -2.0,
                        .source_offset_y_radii = 1.0,
                        .feather = 0.18,
                        .strength = 0.63,
                    }},
                    .strokes = {std::move(stroke)},
                },
        },
        image::AdjustmentNode{
            .node_id = "after-clone",
            .parameters = image::SaturationAdjustment{.factor = 0.93},
        },
    };
}

void resident_gpu_retouch_matches_the_cpu_or_declines() {
    const auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU clone was required but no resident Metal session could be prepared"
        );
        return;
    }

    const auto nodes = clone_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto before = preparation.session->stats();
    const auto first = preparation.session->render(nodes, plan, true);
    const auto after_first = preparation.session->stats();
    const auto second = preparation.session->render(nodes, plan, true);
    const auto after_second = preparation.session->stats();
    auto strength_changed = nodes;
    std::get<image::SpotHealAdjustment>(strength_changed[1U].parameters)
        .strokes.front().strength = 0.72;
    const auto adjusted_strength = preparation.session->render(
        strength_changed,
        image::compile_edit_execution_plan(strength_changed),
        true
    );
    const auto after_strength = preparation.session->stats();
    expect(
        first.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && first.output.has_value() && first.output->analyzed_linear.has_value()
            && second.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && second.output.has_value() && second.output->analyzed_linear.has_value()
            && adjusted_strength.status
                   == image::detail::WarmEditGpuSession::RenderStatus::completed,
        "continuous Clone remains inside one resident Metal transaction"
    );
    expect(
        after_first.retouch_geometry_resource_upload_count
                == before.retouch_geometry_resource_upload_count + 1U
            && after_second.retouch_geometry_resource_upload_count
                   == after_first.retouch_geometry_resource_upload_count
            && after_strength.retouch_geometry_resource_upload_count
                   == after_second.retouch_geometry_resource_upload_count
            && after_second.resource_cache_hit_count > after_first.resource_cache_hit_count,
        "retouch geometry uploads once and is reused while strength changes"
    );
    if (first.output && first.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool parity =
            linear_close(*first.output->analyzed_linear, cpu.pixels, maximum_error, 3.0e-4);
        if (!parity) {
            std::cerr << "Clone warm linear parity max=" << maximum_error << '\n';
        }
        expect(parity, "resident Metal Clone tracks the ordered CPU source-snapshot oracle");
    }

    auto heal = nodes;
    std::get<image::SpotHealAdjustment>(heal[1U].parameters).spots.front().mode =
        image::SpotRepairMode::heal;
    const auto healed =
        preparation.session->render(heal, image::compile_edit_execution_plan(heal), true);
    expect(
        healed.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && healed.output.has_value() && healed.output->analyzed_linear.has_value(),
        "mixed ordered Heal and Clone regions remain inside one resident Metal transaction"
    );
    if (healed.output && healed.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            heal,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool parity =
            linear_close(*healed.output->analyzed_linear, cpu.pixels, maximum_error, 3.5e-3);
        if (!parity) {
            std::cerr << "Heal warm linear parity max=" << maximum_error << '\n';
            double observed = 0.0;
            std::uint32_t observed_x = 0U;
            std::uint32_t observed_y = 0U;
            std::size_t observed_channel = 0U;
            const std::size_t gpu_row =
                healed.output->analyzed_linear->row_stride_bytes / sizeof(float);
            const std::size_t cpu_row = cpu.pixels.row_stride_bytes / sizeof(float);
            for (std::uint32_t y = 0U; y < source.dimensions.height; ++y) {
                for (std::uint32_t x = 0U; x < source.dimensions.width; ++x) {
                    for (std::size_t channel = 0U; channel < 3U; ++channel) {
                        const double error = std::abs(
                            static_cast<double>(healed.output->analyzed_linear->samples
                                                    [static_cast<std::size_t>(y) * gpu_row
                                                     + static_cast<std::size_t>(x) * 3U + channel])
                            - static_cast<double>(
                                cpu.pixels.samples
                                    [static_cast<std::size_t>(y) * cpu_row
                                     + static_cast<std::size_t>(x) * 3U + channel]
                            )
                        );
                        if (error > observed) {
                            observed = error;
                            observed_x = x;
                            observed_y = y;
                            observed_channel = channel;
                        }
                    }
                }
            }
            const std::size_t gpu_index = static_cast<std::size_t>(observed_y) * gpu_row
                                          + static_cast<std::size_t>(observed_x) * 3U
                                          + observed_channel;
            const std::size_t cpu_index = static_cast<std::size_t>(observed_y) * cpu_row
                                          + static_cast<std::size_t>(observed_x) * 3U
                                          + observed_channel;
            std::cerr << "Heal max x=" << observed_x << " y=" << observed_y
                      << " channel=" << observed_channel
                      << " GPU=" << healed.output->analyzed_linear->samples[gpu_index]
                      << " CPU=" << cpu.pixels.samples[cpu_index] << '\n';
        }
        expect(parity, "resident Metal Heal tracks the robust screened-Poisson CPU oracle");
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

void benchmark_retouch_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_RETOUCH_BENCHMARK") == nullptr) {
        return;
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto source = make_random_image(dimensions.width, dimensions.height, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        std::cerr << "BENCH Clone unavailable: " << preparation.diagnostic << '\n';
        ++failures;
        return;
    }
    const auto nodes = clone_nodes(96U);
    const auto plan = image::compile_edit_execution_plan(nodes);
    std::uint64_t checksum = 0U;

    const double first_resident = median_milliseconds(1U, [&]() {
        auto rendered = preparation.session->render(nodes, plan, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
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
    std::cout << std::fixed << std::setprecision(3) << "BENCH continuous Clone " << dimensions.width
              << 'x' << dimensions.height << " points=96"
              << " CPU-adjust+display=" << cpu << "ms"
              << " first-resident-Metal=" << first_resident << "ms"
              << " resident-Metal=" << resident << "ms"
              << " speedup=" << cpu / resident << 'x' << " checksum=" << checksum << '\n';

    auto heal_nodes = nodes;
    auto& repair = std::get<image::SpotHealAdjustment>(heal_nodes[1U].parameters);
    for (auto& spot : repair.spots) {
        spot.mode = image::SpotRepairMode::heal;
    }
    for (auto& stroke : repair.strokes) {
        stroke.mode = image::SpotRepairMode::heal;
    }
    const auto heal_plan = image::compile_edit_execution_plan(heal_nodes);
    const double first_resident_heal = median_milliseconds(1U, [&]() {
        auto rendered = preparation.session->render(heal_nodes, heal_plan, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    constexpr std::size_t heal_iterations = 2U;
    const double cpu_heal = median_milliseconds(heal_iterations, [&]() {
        auto adjusted = image::execute_adjustment_nodes_with_backend(
            source,
            heal_nodes,
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
    const double resident_heal = median_milliseconds(heal_iterations, [&]() {
        auto rendered = preparation.session->render(heal_nodes, heal_plan, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH continuous Heal " << dimensions.width
              << 'x' << dimensions.height << " points=96"
              << " CPU-adjust+display=" << cpu_heal << "ms"
              << " first-resident-Metal=" << first_resident_heal << "ms"
              << " resident-Metal=" << resident_heal << "ms"
              << " speedup=" << cpu_heal / resident_heal << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int run_resident_gpu_retouch_contract() {
    failures = 0;
    resident_gpu_retouch_matches_the_cpu_or_declines();
    benchmark_retouch_when_requested();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

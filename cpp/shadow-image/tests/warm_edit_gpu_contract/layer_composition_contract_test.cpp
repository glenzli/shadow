#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../../src/proxy/warm_edit_gpu.hpp"

#include <array>
#include <chrono>
#include <cmath>
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

[[nodiscard]] std::array<image::AdjustmentLayer, 3U> photographic_layers() {
    return {
        image::AdjustmentLayer{
            .layer_id = "global-opacity",
            .opacity = 0.63,
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "global-exposure",
                        .parameters = image::ExposureAdjustment{.stops = 0.24},
                    },
                    image::AdjustmentNode{
                        .node_id = "global-saturation",
                        .parameters = image::SaturationAdjustment{.factor = 0.91},
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "linear-sky",
            .opacity = 0.78,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::linear_gradient,
                    .x0 = 0.18,
                    .y0 = 0.12,
                    .x1 = 0.82,
                    .y1 = 0.71,
                    .invert = true,
                },
            .nodes =
                {
                    image::AdjustmentNode{
                        .node_id = "sky-exposure",
                        .parameters = image::ExposureAdjustment{.stops = -0.31},
                    },
                    image::AdjustmentNode{
                        .node_id = "sky-white-balance",
                        .parameters =
                            image::RgbWhiteBalanceAdjustment{
                                .temperature = -0.07,
                                .tint = 0.04,
                            },
                    },
                },
        },
        image::AdjustmentLayer{
            .layer_id = "radial-subject-detail",
            .opacity = 0.84,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::radial_gradient,
                    .x0 = 0.57,
                    .y0 = 0.46,
                    .radius_x = 0.31,
                    .radius_y = 0.24,
                    .feather = 0.58,
                },
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "subject-detail",
                    .parameter_schema_version = image::detail_effects_parameter_schema_version,
                    .implementation_version = image::color_grading_implementation_version,
                    .parameters = image::SharpenAdjustment{
                        .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                        .clarity = 0.24,
                        .texture = 0.31,
                        .local_contrast = 0.19,
                        .local_contrast_scale = 0.62,
                    },
                },
            },
        },
    };
}

void resident_layers_match_the_cpu_oracle() {
    const auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU layer composition was required but no resident Metal session could be prepared"
        );
        return;
    }

    const auto layers = photographic_layers();
    const auto gpu = preparation.session->render_layers(layers, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "sequential opacity, linear, radial and neighborhood layers complete on resident Metal"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_layers(source, layers);
        double maximum_error = 0.0;
        const bool parity = linear_close(*gpu.output->analyzed_linear, cpu, maximum_error, 1.2e-3);
        if (!parity) {
            std::cerr << "Layer composition warm linear parity max=" << maximum_error << '\n';
        }
        expect(
            parity,
            "resident Metal layer snapshots and spatial blends track the sequential CPU oracle"
        );
    }
}

void indexed_brush_masks_match_the_continuous_cpu_oracle_and_reuse_resources() {
    const auto source = make_random_image(257U, 149U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "indexed GPU brush composition was required but no Metal session is available"
        );
        return;
    }
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "brush",
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::brush,
                    .radius_x = 0.08,
                    .feather = 0.5,
                    .points =
                        {
                            {.x = 0.08, .y = 0.18, .begins_stroke = true},
                            {.x = 0.28, .y = 0.32},
                            {.x = 0.52, .y = 0.48},
                            {.x = 0.76, .y = 0.67},
                            {.x = 0.84, .y = 0.18, .begins_stroke = true},
                            {.x = 0.62, .y = 0.29},
                        },
                },
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "brush-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.4},
                },
                image::AdjustmentNode{
                    .node_id = "brush-saturation",
                    .parameters = image::SaturationAdjustment{.factor = 0.86},
                },
            },
        },
    };
    const auto before = preparation.session->stats();
    const auto first = preparation.session->render_layers(layers, true);
    expect(
        first.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && first.output.has_value() && first.output->analyzed_linear.has_value(),
        "continuous multi-stroke brush layers complete through the indexed resident Metal path"
    );
    if (first.output && first.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_layers(source, layers);
        double maximum_error = 0.0;
        const bool parity =
            linear_close(*first.output->analyzed_linear, cpu, maximum_error, 1.5e-3);
        if (!parity) {
            std::cerr << "Indexed brush warm linear parity max=" << maximum_error << '\n';
        }
        expect(
            parity,
            "indexed Metal capsule coverage tracks the continuous point-to-segment CPU oracle"
        );
    }
    const auto after_first = preparation.session->stats();
    const auto second = preparation.session->render_layers(layers, false);
    const auto after_second = preparation.session->stats();
    const auto third = preparation.session->render_layers(layers, false);
    const auto after_third = preparation.session->stats();
    expect(
        second.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && second.output.has_value()
            && third.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && third.output.has_value()
            && after_first.brush_index_resource_upload_count
                   == before.brush_index_resource_upload_count + 1U
            && after_second.brush_index_resource_upload_count
                   == after_first.brush_index_resource_upload_count
            && after_third.brush_index_resource_upload_count
                   == after_second.brush_index_resource_upload_count
            && after_third.gpu_buffer_allocation_count == after_second.gpu_buffer_allocation_count
            && after_third.resource_cache_hit_count > after_second.resource_cache_hit_count,
        "both slots warm once, then repeated slider renders reuse the exact resident brush index"
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

void benchmark_gradient_layer_transaction_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_LAYER_BENCHMARK") == nullptr) {
        return;
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto source = make_random_image(dimensions.width, dimensions.height, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        std::cerr << "BENCH gradient layers unavailable: " << preparation.diagnostic << '\n';
        ++failures;
        return;
    }
    const auto layers = photographic_layers();
    std::uint64_t checksum = 0U;
    const double first_resident = median_milliseconds(1U, [&]() {
        auto rendered = preparation.session->render_layers(layers, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    static_cast<void>(
        image::execute_adjustment_layers(source, layers, {.full_dimensions = dimensions})
    );
    constexpr std::size_t iterations = 3U;
    const double cpu = median_milliseconds(iterations, [&]() {
        auto adjusted =
            image::execute_adjustment_layers(source, layers, {.full_dimensions = dimensions});
        auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
            adjusted,
            {.target_dimensions = dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        checksum += displayed.bytes[displayed.bytes.size() / 2U];
    });
    const double resident = median_milliseconds(iterations, [&]() {
        auto rendered = preparation.session->render_layers(layers, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH gradient layer transaction "
              << dimensions.width << 'x' << dimensions.height << " CPU-adjust+display=" << cpu
              << "ms"
              << " first-resident-Metal=" << first_resident << "ms"
              << " resident-Metal=" << resident << "ms"
              << " speedup=" << cpu / resident << 'x' << " checksum=" << checksum << '\n';
}

void benchmark_indexed_brush_transaction_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_BRUSH_BENCHMARK") == nullptr) {
        return;
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto source = make_random_image(dimensions.width, dimensions.height, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        std::cerr << "BENCH indexed brush unavailable: " << preparation.diagnostic << '\n';
        ++failures;
        return;
    }
    image::LocalMask brush{
        .kind = image::LocalMaskKind::brush,
        .radius_x = 0.025,
        .feather = 0.52,
    };
    constexpr std::size_t points_per_stroke = 48U;
    for (std::size_t stroke = 0U; stroke < 2U; ++stroke) {
        for (std::size_t point = 0U; point < points_per_stroke; ++point) {
            const double t =
                static_cast<double>(point) / static_cast<double>(points_per_stroke - 1U);
            brush.points.push_back(
                image::LocalMaskPoint{
                    .x = 0.05 + t * 0.9,
                    .y = 0.28 + static_cast<double>(stroke) * 0.36 + std::sin(t * 8.0) * 0.08,
                    .begins_stroke = point == 0U,
                }
            );
        }
    }
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "benchmark-brush",
            .opacity = 0.82,
            .mask = std::move(brush),
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "benchmark-brush-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.33},
                },
                image::AdjustmentNode{
                    .node_id = "benchmark-brush-saturation",
                    .parameters = image::SaturationAdjustment{.factor = 0.9},
                },
            },
        },
    };
    std::uint64_t checksum = 0U;
    const double first_resident = median_milliseconds(1U, [&]() {
        auto rendered = preparation.session->render_layers(layers, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    static_cast<void>(
        image::execute_adjustment_layers(source, layers, {.full_dimensions = dimensions})
    );
    constexpr std::size_t iterations = 3U;
    const double cpu = median_milliseconds(iterations, [&]() {
        auto adjusted =
            image::execute_adjustment_layers(source, layers, {.full_dimensions = dimensions});
        auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
            adjusted,
            {.target_dimensions = dimensions},
            image::DisplayOutputBackendMode::cpu
        );
        checksum += displayed.bytes[displayed.bytes.size() / 2U];
    });
    const double resident = median_milliseconds(iterations, [&]() {
        auto rendered = preparation.session->render_layers(layers, false);
        if (!rendered.output.has_value()) {
            throw std::runtime_error(rendered.diagnostic);
        }
        checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH indexed brush transaction "
              << dimensions.width << 'x' << dimensions.height << " points=" << 96U
              << " CPU-adjust+display=" << cpu << "ms"
              << " first-resident-Metal=" << first_resident << "ms"
              << " resident-Metal=" << resident << "ms"
              << " speedup=" << cpu / resident << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int run_resident_gpu_layer_composition_contract() {
    failures = 0;
    resident_layers_match_the_cpu_oracle();
    indexed_brush_masks_match_the_continuous_cpu_oracle_and_reuse_resources();
    benchmark_gradient_layer_transaction_when_requested();
    benchmark_indexed_brush_transaction_when_requested();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

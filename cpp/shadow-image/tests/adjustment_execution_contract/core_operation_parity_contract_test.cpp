#include "adjustment_execution_contract_cases.hpp"
#include "execution_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/display_output.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <numeric>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract {

namespace {

using parity_fixture::close_to_cpu;
using parity_fixture::make_image;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::array<image::AdjustmentNode, 4U> core_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "white-balance",
            .parameters =
                image::RgbWhiteBalanceAdjustment{
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
            .parameters =
                image::ContrastAdjustment{
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
            metal.valid() && metal.backend == image::AdjustmentBackend::metal && !metal.fell_back
                && close_to_cpu(metal.pixels, cpu, error),
            "Metal preserves one of the 24 core-operation orders"
        );
        worst_error = std::max(worst_error, error);
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));
    expect(permutation_count == 24U, "all 24 core-operation permutations execute");
    std::cout << "Metal adjustment 24-order maximum absolute error: " << worst_error << '\n';

    image::ImageCompletionPatch first{
        .raster_width = 4U,
        .raster_height = 4U,
        .coordinate_width = 37U,
        .coordinate_height = 23U,
        .bounds_left = 0.20,
        .bounds_top = 0.20,
        .bounds_right = 0.70,
        .bounds_bottom = 0.80,
        .strength = 0.83,
        .rgba8 = std::vector<std::uint8_t>(4U * 4U * 4U),
    };
    for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
        first.rgba8[pixel * 4U] = static_cast<std::uint8_t>(40U + pixel * 9U);
        first.rgba8[pixel * 4U + 1U] = 180U;
        first.rgba8[pixel * 4U + 2U] = 220U;
        first.rgba8[pixel * 4U + 3U] = pixel % 3U == 0U ? 128U : 255U;
    }
    auto second = first;
    second.bounds_left = 0.40;
    second.bounds_top = 0.35;
    second.bounds_right = 0.85;
    second.bounds_bottom = 0.90;
    second.strength = 0.54;
    std::fill(second.rgba8.begin(), second.rgba8.end(), 96U);
    const std::array completion_nodes{
        image::AdjustmentNode{
            .node_id = "grade-before-completion",
            .parameters = image::ExposureAdjustment{.stops = -0.85},
        },
        image::AdjustmentNode{
            .node_id = "two-accepted-regions",
            .parameters = image::ImageCompletionAdjustment{
                .patches = {std::move(first), std::move(second)},
            },
        },
    };
    const auto completion_cpu = image::execute_adjustment_nodes(input, completion_nodes);
    const auto completion_metal = image::execute_adjustment_nodes_with_backend(
        input,
        completion_nodes,
        {},
        image::AdjustmentBackendMode::metal
    );
    double completion_error = 0.0;
    expect(
        completion_metal.backend == image::AdjustmentBackend::metal && !completion_metal.fell_back
            && close_to_cpu(completion_metal.pixels, completion_cpu, completion_error, 2.0e-4),
        "two overlapping AI completion regions follow their existing post-grade order on Metal"
    );

    auto linear_nodes = completion_nodes;
    auto& regions = std::get<image::ImageCompletionAdjustment>(linear_nodes[1].parameters).patches;
    for (auto& region : regions) {
        region.linear_rgba_f32 = true;
        region.color_response = {1.2, 0.1, -0.05, -0.1, 0.9, 0.02, 0.05, 0.1, 0.8};
        region.rgba8.clear();
        for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
            const float alpha = pixel % 3U == 0U ? 0.0F : 0.7F;
            for (const float sample : std::array{2.0F + float(pixel) * 0.3F, -0.03F, 0.4F, alpha}) {
                const auto bits = std::bit_cast<std::uint32_t>(sample);
                for (unsigned shift = 0U; shift < 32U; shift += 8U)
                    region.rgba8.push_back(static_cast<std::uint8_t>(bits >> shift));
            }
        }
    }
    std::swap(linear_nodes[0], linear_nodes[1]);
    const auto linear_cpu = image::execute_adjustment_nodes(input, linear_nodes);
    const auto linear_metal = image::execute_adjustment_nodes_with_backend(
        input,
        linear_nodes,
        {},
        image::AdjustmentBackendMode::metal
    );
    double linear_error = 0.0;
    expect(
        linear_metal.backend == image::AdjustmentBackend::metal && !linear_metal.fell_back
            && close_to_cpu(linear_metal.pixels, linear_cpu, linear_error, 2.0e-4),
        "linear HDR completion with erased texels and downstream exposure matches CPU on Metal"
    );
    const auto detail_input = make_image(11U, 7U);
    const image::AdjustmentExecutionContext detail_context{
        .origin_x = 13U,
        .origin_y = 9U,
        .full_dimensions = {.width = 37U, .height = 23U},
    };
    const auto detail_cpu =
        image::execute_adjustment_nodes(detail_input, completion_nodes, detail_context);
    const auto detail_metal = image::execute_adjustment_nodes_with_backend(
        detail_input,
        completion_nodes,
        detail_context,
        image::AdjustmentBackendMode::metal
    );

    const auto linear_detail_cpu =
        image::execute_adjustment_nodes(detail_input, linear_nodes, detail_context);
    const auto linear_detail_metal = image::execute_adjustment_nodes_with_backend(
        detail_input,
        linear_nodes,
        detail_context,
        image::AdjustmentBackendMode::metal
    );
    double linear_detail_error = 0.0;
    expect(
        linear_detail_metal.backend == image::AdjustmentBackend::metal
            && !linear_detail_metal.fell_back
            && close_to_cpu(
                linear_detail_metal.pixels,
                linear_detail_cpu,
                linear_detail_error,
                2.0e-4
            ),
        "linear completion subpixel maps preserve detail origin on Metal"
    );
    double detail_error = 0.0;
    expect(
        detail_metal.backend == image::AdjustmentBackend::metal && !detail_metal.fell_back
            && close_to_cpu(detail_metal.pixels, detail_cpu, detail_error, 2.0e-4),
        "AI completion tile coordinates and clipped bounds match CPU at nonzero origin"
    );
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
                .parameters =
                    image::RgbWhiteBalanceAdjustment{
                        .temperature = iteration == 0U ? 1.0 : white_balance(generator),
                        .tint = iteration == 1U ? -1.0 : white_balance(generator),
                    },
            },
            image::AdjustmentNode{
                .node_id = "random-exposure",
                .parameters =
                    image::ExposureAdjustment{
                        .stops = exposure(generator),
                    },
            },
            image::AdjustmentNode{
                .node_id = "random-contrast",
                .parameters =
                    image::ContrastAdjustment{
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
                } else if (
                    const auto* value = std::get_if<image::ExposureAdjustment>(&node.parameters)
                ) {
                    std::cerr << '(' << value->stops << ')';
                } else if (
                    const auto* value = std::get_if<image::ContrastAdjustment>(&node.parameters)
                ) {
                    std::cerr << '(' << value->factor << ',' << value->pivot << ')';
                } else if (
                    const auto* value = std::get_if<image::SaturationAdjustment>(&node.parameters)
                ) {
                    std::cerr << '(' << value->factor << ')';
                }
            }
            std::cerr << '\n';
        }
        expect(
            metal.backend == image::AdjustmentBackend::metal && parity,
            "random, endpoint, factor-zero and super-white parameters match CPU"
        );
        worst_error = std::max(worst_error, error);
        const auto cpu_display = image::render_linear_srgb_to_display_srgb8_cpu_reference(
            cpu,
            image::DisplayOutputRequest{.target_dimensions = cpu.dimensions}
        );
        const auto metal_display = image::render_linear_srgb_to_display_srgb8_cpu_reference(
            metal.pixels,
            image::DisplayOutputRequest{
                .target_dimensions = metal.pixels.dimensions,
            }
        );
        for (std::size_t index = 0U; index < cpu_display.bytes.size(); ++index) {
            display_code_errors.push_back(
                static_cast<unsigned int>(std::abs(
                    static_cast<int>(cpu_display.bytes[index])
                    - static_cast<int>(metal_display.bytes[index])
                ))
            );
        }
    }
    std::sort(display_code_errors.begin(), display_code_errors.end());
    const unsigned int maximum_code_error = display_code_errors.back();
    const std::size_t p99_index =
        static_cast<std::size_t>(static_cast<double>(display_code_errors.size() - 1U) * 0.99);
    const unsigned int p99_code_error = display_code_errors[p99_index];
    const double mean_code_error = static_cast<double>(std::accumulate(
                                       display_code_errors.begin(),
                                       display_code_errors.end(),
                                       std::uint64_t{0U}
                                   ))
                                   / static_cast<double>(display_code_errors.size());
    expect(
        maximum_code_error <= 1U && p99_code_error <= 1U && mean_code_error <= 0.02,
        "extreme fp32 adjustment differences stay below one display code"
    );
    std::cout << "Metal adjustment randomized maximum absolute error: " << worst_error
              << "; display RGB8 max=" << maximum_code_error << ", p99=" << p99_code_error
              << ", mean=" << mean_code_error << '\n';
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
                static_cast<void>(
                    image::execute_adjustment_nodes_with_backend(input, nodes, {}, backend)
                );
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
        std::cout << "Adjustment benchmark " << dimensions.width << 'x' << dimensions.height
                  << ": CPU p50=" << cpu_p50 << " ms/p95=" << cpu_p95
                  << " ms, Metal p50=" << metal_p50 << " ms/p95=" << metal_p95
                  << " ms, p50 speedup=" << cpu_p50 / metal_p50 << "x\n";
    }
}
} // namespace

int run_every_core_order_matches_the_cpu_oracle() {
    failures = 0;
    every_core_order_matches_the_cpu_oracle();
    return failures;
}

int run_randomized_and_endpoint_parameters_match_the_cpu_oracle() {
    failures = 0;
    randomized_and_endpoint_parameters_match_the_cpu_oracle();
    return failures;
}

int run_repeated_nodes_and_concurrent_renders_are_deterministic() {
    failures = 0;
    repeated_nodes_and_concurrent_renders_are_deterministic();
    return failures;
}

int run_optional_true_machine_benchmark() {
    failures = 0;
    optional_true_machine_benchmark();
    return failures;
}

} // namespace shadow::image::adjustment_execution_contract

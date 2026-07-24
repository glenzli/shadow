#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/display_output.hpp>

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

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

class ScopedEnvironment final {
public:
    ScopedEnvironment(const std::string_view name, const std::string_view value)
        : name_(name) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
            had_previous_ = true;
        }
#if defined(_WIN32)
        static_cast<void>(_putenv_s(name_.c_str(), std::string(value).c_str()));
#else
        static_cast<void>(setenv(name_.c_str(), std::string(value).c_str(), 1));
#endif
    }

    ~ScopedEnvironment() {
#if defined(_WIN32)
        static_cast<void>(_putenv_s(
            name_.c_str(),
            had_previous_ ? previous_.c_str() : ""
        ));
#else
        if (had_previous_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

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
    auto curve = image::OklabLightnessToneCurve{};
    curve.lightness.points = {{0.0, -0.05}, {0.45, 0.50}, {1.0, 1.10}};
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "gpu-prefix",
            .parameters = image::ExposureAdjustment{.stops = 0.5},
        },
        image::AdjustmentNode{
            .node_id = "unsupported-middle",
            .parameters = std::move(curve),
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
                image::selective_tone_v3_parameter_schema_version,
            .implementation_version =
                image::selective_tone_v3_implementation_version,
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
    malformed_disabled_nodes_fail_before_backend_selection();
    backend_availability_and_resource_failure_are_explicit();
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

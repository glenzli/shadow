#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/display_output.hpp>

#include "../src/proxy/warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <ranges>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
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

[[nodiscard]] image::FloatRgbImage make_random_image(
    const std::uint32_t width,
    const std::uint32_t height,
    const bool padded
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
    std::mt19937 generator(0x914dU);
    std::uniform_real_distribution<float> samples(-0.15F, 2.25F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * row_floats;
        for (std::size_t index = 0U; index < active_row; ++index) {
            result.samples[row + index] = samples(generator);
        }
    }
    return result;
}

[[nodiscard]] image::FloatRgbImage make_boundary_image() {
    image::FloatRgbImage result{
        .dimensions = {5U, 1U},
        .row_stride_bytes = 5U * 3U * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_srgb(),
        .samples = {
            0.0F, 0.0F, 0.0F,
            1.0F, 1.0F, 1.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 1.0F,
        },
    };
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
        .title = "Warm GPU advanced contract",
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
                image::detail_effects_v3_parameter_schema_version,
            .implementation_version =
                image::color_grading_v3_implementation_version,
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

[[nodiscard]] bool linear_close(
    const image::FloatRgbImage& actual,
    const image::FloatRgbImage& expected,
    double& maximum_error,
    const double relative_tolerance = 6.0e-5
) {
    if (actual.dimensions != expected.dimensions
        || actual.row_stride_bytes % sizeof(float) != 0U
        || expected.row_stride_bytes % sizeof(float) != 0U) {
        return false;
    }
    maximum_error = 0.0;
    bool close = true;
    const std::size_t actual_row_floats =
        actual.row_stride_bytes / sizeof(float);
    const std::size_t expected_row_floats =
        expected.row_stride_bytes / sizeof(float);
    const std::size_t active_row =
        static_cast<std::size_t>(actual.dimensions.width) * 3U;
    if (actual_row_floats < active_row || expected_row_floats < active_row
        || actual.samples.size()
            != actual_row_floats * actual.dimensions.height
        || expected.samples.size()
            != expected_row_floats * expected.dimensions.height) {
        return false;
    }
    for (std::uint32_t y = 0U; y < actual.dimensions.height; ++y) {
        const std::size_t actual_row =
            static_cast<std::size_t>(y) * actual_row_floats;
        const std::size_t expected_row =
            static_cast<std::size_t>(y) * expected_row_floats;
        for (std::size_t x = 0U; x < active_row; ++x) {
            const double reference = expected.samples[expected_row + x];
            const double difference = std::abs(
                static_cast<double>(actual.samples[actual_row + x]) - reference
            );
            maximum_error = std::max(maximum_error, difference);
            if (difference
                > relative_tolerance * std::max(1.0, std::abs(reference))) {
                close = false;
            }
        }
    }
    return close;
}

struct Rgb8Difference final {
    std::uint8_t maximum = 0U;
    std::uint8_t p99 = 0U;
};

[[nodiscard]] Rgb8Difference rgb8_difference(
    const std::span<const std::uint8_t> actual,
    const std::span<const std::uint8_t> expected
) {
    if (actual.size() != expected.size() || actual.empty()) {
        return {.maximum = 255U, .p99 = 255U};
    }
    std::vector<std::uint8_t> differences(actual.size());
    for (std::size_t index = 0U; index < actual.size(); ++index) {
        differences[index] = static_cast<std::uint8_t>(
            std::abs(
                static_cast<int>(actual[index])
                - static_cast<int>(expected[index])
            )
        );
    }
    const auto maximum = *std::ranges::max_element(differences);
    const std::size_t p99_index =
        static_cast<std::size_t>(0.99 * static_cast<double>(differences.size() - 1U));
    std::ranges::nth_element(
        differences,
        differences.begin() + static_cast<std::ptrdiff_t>(p99_index)
    );
    return {
        .maximum = maximum,
        .p99 = differences[p99_index],
    };
}

void resident_backend_matches_cpu_oracle() {
    const auto source = make_random_image(257U, 129U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "session-resident warm Metal was required but could not be prepared"
        );
        return;
    }
    const auto initial_stats = preparation.session->stats();
    expect(
        initial_stats.resident
            && initial_stats.source_upload_count == 1U
            && initial_stats.gpu_buffer_allocation_count == 10U
            && initial_stats.render_count == 0U,
        "resident backend starts with one source, one dummy side table, and two four-buffer slots"
    );

    auto original_nodes = core_nodes();
    std::array<std::size_t, 4U> order{0U, 1U, 2U, 3U};
    std::size_t permutation_count = 0U;
    do {
        std::array<image::AdjustmentNode, 4U> nodes{
            original_nodes[order[0]],
            original_nodes[order[1]],
            original_nodes[order[2]],
            original_nodes[order[3]],
        };
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu_adjusted = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_with_backend(
                cpu_adjusted.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
        auto gpu = preparation.session->render(nodes, plan, true);
        expect(
            gpu.output.has_value() && gpu.output->analyzed_linear.has_value(),
            "every supported node order completes on the resident backend"
        );
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            continue;
        }
        expect(
            gpu.output->analyzed_linear->row_stride_bytes
                    == static_cast<std::size_t>(source.dimensions.width)
                        * 3U * sizeof(float)
                && gpu.output->analyzed_linear->samples.size()
                    == static_cast<std::size_t>(source.dimensions.pixel_count()) * 3U,
            "resident analyzed output is packed and contains no unwritten source padding"
        );
        double maximum_linear_error = 0.0;
        const auto rgb8 = rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
        const bool linear_matches = linear_close(
            *gpu.output->analyzed_linear,
            cpu_adjusted.pixels,
            maximum_linear_error
        );
        if (!linear_matches) {
            std::cerr << "Warm linear parity order="
                      << order[0] << order[1] << order[2] << order[3]
                      << " max=" << maximum_linear_error << '\n';
        }
        expect(
            linear_matches,
            "resident fused linear output stays within the established fp32 tolerance"
        );
        expect(
            rgb8.maximum <= 1U && rgb8.p99 == 0U,
            "resident fused display output differs by at most one code with p99 exact"
        );
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));
    expect(permutation_count == 24U, "all 24 core-operation orders were validated");

    const auto boundary = make_boundary_image();
    auto boundary_preparation = image::detail::prepare_warm_edit_gpu_session(boundary);
    expect(
        boundary_preparation.session != nullptr,
        "boundary source can prepare a resident session"
    );
    if (boundary_preparation.session) {
        const std::array neutral_nodes{
            image::AdjustmentNode{
                .node_id = "neutral",
                .parameters = image::ExposureAdjustment{},
            },
        };
        const auto neutral_plan = image::compile_edit_execution_plan(neutral_nodes);
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_with_backend(
                boundary,
                {.target_dimensions = boundary.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
        const auto gpu =
            boundary_preparation.session->render(neutral_nodes, neutral_plan, true);
        expect(
            gpu.output.has_value()
                && !gpu.output->had_active_adjustments
                && gpu.output->analyzed_linear.has_value()
                && rgb8_difference(gpu.output->rgb8, cpu_display.bytes).maximum <= 1U,
            "neutral boundary display stays within one RGB8 code and records no adjustment"
        );
    }

    const auto allocation_snapshot = preparation.session->stats();
    const auto nodes = core_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    auto first = std::async(std::launch::async, [&]() {
        return preparation.session->render(nodes, plan, false);
    });
    auto second = std::async(std::launch::async, [&]() {
        return preparation.session->render(nodes, plan, false);
    });
    const auto first_result = first.get();
    const auto second_result = second.get();
    const auto after_concurrent = preparation.session->stats();
    expect(
        first_result.output.has_value() && second_result.output.has_value()
            && first_result.output->rgb8 == second_result.output->rgb8
            && !first_result.output->analyzed_linear.has_value()
            && !second_result.output->analyzed_linear.has_value()
            && after_concurrent.source_upload_count
                == allocation_snapshot.source_upload_count
            && after_concurrent.gpu_buffer_allocation_count
                == allocation_snapshot.gpu_buffer_allocation_count
            && after_concurrent.render_count == allocation_snapshot.render_count + 2U
            && after_concurrent.completed_render_count
                == allocation_snapshot.completed_render_count + 2U
            && after_concurrent.peak_concurrent_renders >= 1U
            && after_concurrent.peak_concurrent_renders <= 2U,
        "concurrent no-analysis renders reuse fixed slots and skip linear readback"
    );

    {
        const auto before_failure = preparation.session->stats();
        const ScopedEnvironment injected_failure(
            "SHADOW_TEST_WARM_METAL_FORCE_FAILURE",
            "1"
        );
        const auto failure = preparation.session->render(nodes, plan, false);
        const auto after_failure = preparation.session->stats();
        expect(
            !failure.output.has_value() && !failure.diagnostic.empty()
                && after_failure.render_count == before_failure.render_count
                && after_failure.gpu_buffer_allocation_count
                    == before_failure.gpu_buffer_allocation_count,
            "injected warm failure declines before a slot and never allocates transient buffers"
        );
    }
}

void advanced_resources_match_cpu_and_reuse_side_table_uploads() {
    const auto source = make_random_image(137U, 83U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "advanced resident Metal was required but could not be prepared"
        );
        return;
    }

    const auto initial = preparation.session->stats();
    expect(
        initial.curve_resource_upload_count == 0U
            && initial.lut_resource_upload_count == 0U
            && initial.resource_cache_hit_count == 0U,
        "a new warm session has an empty advanced-resource cache"
    );
    const auto render_and_compare = [&source, &preparation](
        const std::span<const image::AdjustmentNode> nodes,
        const std::string_view description
    ) {
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        const auto cpu_display =
            image::render_linear_srgb_to_display_srgb8_with_backend(
                cpu.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
        const auto gpu = preparation.session->render(nodes, plan, true);
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            std::cerr << "Advanced warm render failed: " << gpu.diagnostic << '\n';
            expect(false, description);
            return;
        }
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            8.0e-5
        );
        const auto display_difference =
            rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
        if (!linear_parity) {
            std::cerr << "Advanced warm linear parity max="
                      << maximum_error << '\n';
        }
        expect(
            linear_parity
                && display_difference.maximum <= 1U
                && display_difference.p99 <= 1U,
            description
        );
    };

    auto nodes = advanced_nodes();
    render_and_compare(nodes, "the first advanced warm render matches the CPU oracle");
    const auto after_first = preparation.session->stats();
    expect(
        after_first.curve_resource_upload_count
                == initial.curve_resource_upload_count + 1U
            && after_first.lut_resource_upload_count
                == initial.lut_resource_upload_count + 1U
            && after_first.resource_cache_hit_count
                == initial.resource_cache_hit_count
            && after_first.gpu_buffer_allocation_count
                == initial.gpu_buffer_allocation_count + 2U
            && after_first.resident_bytes > initial.resident_bytes,
        "the first curve and LUT each publish one resident side-table buffer"
    );

    render_and_compare(nodes, "an identical advanced warm render remains CPU-equivalent");
    const auto after_identical = preparation.session->stats();
    expect(
        after_identical.curve_resource_upload_count
                == after_first.curve_resource_upload_count
            && after_identical.lut_resource_upload_count
                == after_first.lut_resource_upload_count
            && after_identical.resource_cache_hit_count
                == after_first.resource_cache_hit_count + 2U
            && after_identical.gpu_buffer_allocation_count
                == after_first.gpu_buffer_allocation_count
            && after_identical.resident_bytes == after_first.resident_bytes,
        "an identical curve and LUT hit both caches without another upload"
    );

    std::get<image::CubeLutAdjustment>(nodes[2U].parameters).intensity = 0.31;
    render_and_compare(nodes, "changing LUT intensity remains CPU-equivalent");
    const auto after_intensity = preparation.session->stats();
    expect(
        after_intensity.curve_resource_upload_count
                == after_identical.curve_resource_upload_count
            && after_intensity.lut_resource_upload_count
                == after_identical.lut_resource_upload_count
            && after_intensity.resource_cache_hit_count
                == after_identical.resource_cache_hit_count + 2U
            && after_intensity.gpu_buffer_allocation_count
                == after_identical.gpu_buffer_allocation_count
            && after_intensity.resident_bytes == after_identical.resident_bytes,
        "LUT intensity is an operation scalar and does not re-upload either side table"
    );

    std::get<image::OklabLightnessToneCurve>(nodes[0U].parameters)
        .lightness.points[2U].y = 0.68;
    render_and_compare(nodes, "changing the Oklab curve remains CPU-equivalent");
    const auto after_curve = preparation.session->stats();
    expect(
        after_curve.curve_resource_upload_count
                == after_intensity.curve_resource_upload_count + 1U
            && after_curve.lut_resource_upload_count
                == after_intensity.lut_resource_upload_count
            && after_curve.resource_cache_hit_count
                == after_intensity.resource_cache_hit_count + 1U
            && after_curve.gpu_buffer_allocation_count
                == after_intensity.gpu_buffer_allocation_count + 1U
            && after_curve.resident_bytes > after_intensity.resident_bytes,
        "changing only the curve publishes one curve buffer and reuses the LUT"
    );
}

void cancellation_is_terminal_without_diagnostic() {
    const auto source = make_random_image(64U, 48U, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    const auto nodes = core_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    std::stop_source cancellation;
    expect(cancellation.request_stop(), "first native stop request succeeds");

    if (preparation.session) {
        const auto before = preparation.session->stats();
        const auto attempt = preparation.session->render(
            nodes,
            plan,
            true,
            cancellation.get_token()
        );
        const auto after = preparation.session->stats();
        expect(
            attempt.status
                == image::detail::WarmEditGpuSession::RenderStatus::cancelled,
            "pre-cancelled resident render has explicit Cancelled status"
        );
        expect(!attempt.output.has_value(), "cancelled resident render returns no pixels");
        expect(attempt.diagnostic.empty(), "cancelled resident render returns no diagnostic");
        expect(
            after.render_count == before.render_count
                && after.completed_render_count == before.completed_render_count,
            "pre-cancelled resident render does not enter or complete a GPU slot"
        );
    }
}

template <typename Callable>
[[nodiscard]] double median_milliseconds(
    const std::size_t iterations,
    Callable&& callable
) {
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        callable();
        const auto finished = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(finished - started).count()
        );
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2U];
}

void benchmark_resident_backend_when_requested() {
    if (std::getenv("SHADOW_TEST_WARM_METAL_BENCHMARK") == nullptr) {
        return;
    }
    for (const auto dimensions : std::array{
             image::Dimensions{1'200U, 800U},
             image::Dimensions{2'048U, 1'365U},
         }) {
        const auto source =
            make_random_image(dimensions.width, dimensions.height, false);
        auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
        if (!preparation.session) {
            std::cerr << "BENCH warm " << dimensions.width << 'x'
                      << dimensions.height << " unavailable: "
                      << preparation.diagnostic << '\n';
            ++failures;
            continue;
        }
        const auto nodes = core_nodes();
        const auto plan = image::compile_edit_execution_plan(nodes);
        std::uint64_t checksum = 0U;

        // Pay lazy process-global pipeline creation and allocator warmup before timing.
        static_cast<void>(preparation.session->render(nodes, plan, false));
        static_cast<void>(image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        ));

        const std::size_t iterations =
            dimensions.width >= 2'000U ? 5U : 7U;
        const double cpu = median_milliseconds(iterations, [&]() {
            auto adjusted = image::execute_adjustment_nodes_with_backend(
                source,
                nodes,
                {.full_dimensions = source.dimensions},
                image::AdjustmentBackendMode::cpu
            );
            auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
                adjusted.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::cpu
            );
            checksum += displayed.bytes[displayed.bytes.size() / 2U];
        });
        const double staged_metal = median_milliseconds(iterations, [&]() {
            auto adjusted = image::execute_adjustment_nodes_with_backend(
                source,
                nodes,
                {.full_dimensions = source.dimensions},
                image::AdjustmentBackendMode::metal
            );
            auto displayed = image::render_linear_srgb_to_display_srgb8_with_backend(
                adjusted.pixels,
                {.target_dimensions = source.dimensions},
                image::DisplayOutputBackendMode::metal
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
        const double resident_with_linear = median_milliseconds(iterations, [&]() {
            auto rendered = preparation.session->render(nodes, plan, true);
            if (!rendered.output.has_value()
                || !rendered.output->analyzed_linear.has_value()) {
                throw std::runtime_error(rendered.diagnostic);
            }
            checksum += rendered.output->rgb8[rendered.output->rgb8.size() / 2U];
        });
        const auto stats = preparation.session->stats();
        std::cout << std::fixed << std::setprecision(3)
                  << "BENCH warm " << dimensions.width << 'x' << dimensions.height
                  << " CPU-adjust+display=" << cpu << "ms"
                  << " staged-Metal=" << staged_metal << "ms"
                  << " resident-no-analysis=" << resident << "ms"
                  << " resident+linear-readback=" << resident_with_linear << "ms"
                  << " speedup-vs-CPU=" << cpu / resident << 'x'
                  << " speedup-vs-staged=" << staged_metal / resident << 'x'
                  << " uploads=" << stats.source_upload_count
                  << " buffers=" << stats.gpu_buffer_allocation_count
                  << " checksum=" << checksum << '\n';
    }
}

} // namespace

int main() {
    resident_backend_matches_cpu_oracle();
    advanced_resources_match_cpu_and_reuse_side_table_uploads();
    cancellation_is_terminal_without_diagnostic();
    benchmark_resident_backend_when_requested();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

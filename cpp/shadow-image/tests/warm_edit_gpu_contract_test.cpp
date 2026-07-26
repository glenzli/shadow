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
                image::detail_effects_parameter_schema_version,
            .implementation_version =
                image::color_grading_implementation_version,
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

[[nodiscard]] std::array<image::AdjustmentNode, 1U> perceptual_nodes() {
    image::PerceptualColorAdjustment parameters;
    parameters.vibrance = 0.31;
    parameters.hue = {0.18, -0.12, 0.07, -0.09, 0.14, -0.16, 0.11, -0.06};
    parameters.saturation = {
        0.13,
        -0.08,
        0.05,
        0.11,
        -0.06,
        0.15,
        -0.09,
        0.07,
    };
    parameters.lightness = {
        -0.06,
        0.09,
        -0.04,
        0.07,
        -0.08,
        0.05,
        -0.03,
        0.10,
    };
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 26.0,
        .width_degrees = 52.0,
        .softness = 0.42,
        .hue_shift_degrees = 14.0,
        .saturation = 0.22,
        .lightness = -0.12,
    };
    parameters.additional_color_ranges = {
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 44.0,
            .width_degrees = 68.0,
            .softness = 0.36,
            .hue_shift_degrees = 31.0,
            .saturation = 0.17,
            .lightness = -0.07,
        },
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 92.0,
            .width_degrees = 57.0,
            .softness = 0.51,
            .hue_shift_degrees = -16.0,
            .saturation = -0.14,
            .lightness = 0.11,
        },
    };
    parameters.selective_color_relative = false;
    parameters.selective_color_lightness_protection = 0.67;
    parameters.selective_color_cmyk = {{
        {{0.10, -0.15, 0.06, 0.04}},
        {{-0.07, 0.12, 0.04, -0.03}},
        {{0.06, -0.05, 0.13, 0.02}},
        {{-0.09, 0.06, -0.11, 0.05}},
        {{0.12, 0.03, -0.07, -0.02}},
        {{-0.04, 0.14, 0.07, 0.03}},
        {{0.02, -0.02, 0.03, 0.07}},
        {{-0.03, 0.04, -0.02, 0.05}},
        {{0.02, -0.01, 0.02, -0.10}},
    }};
    return {
        image::AdjustmentNode{
            .node_id = "warm-perceptual-color",
            .parameter_schema_version =
                image::perceptual_color_parameter_schema_version,
            .implementation_version =
                image::perceptual_color_implementation_version,
            .parameters = std::move(parameters),
        },
    };
}

[[nodiscard]] std::array<image::AdjustmentNode, 1U> color_warper_nodes() {
    image::OklabColorWarperAdjustment parameters;
    for (std::size_t row = 0U; row < image::oklab_color_warper_grid_side; ++row) {
        for (std::size_t column = 0U;
             column < image::oklab_color_warper_grid_side;
             ++column) {
            auto& point = parameters.control_points[
                row * image::oklab_color_warper_grid_side + column
            ];
            point.a_offset = 0.008 * static_cast<double>(column) - 0.016;
            point.b_offset = 0.007 * static_cast<double>(row) - 0.014;
        }
    }
    parameters.strength = 0.73;
    return {
        image::AdjustmentNode{
            .node_id = "warm-oklab-color-warper",
            .parameter_schema_version = image::oklab_color_warper_parameter_schema_version,
            .implementation_version = image::oklab_color_warper_implementation_version,
            .parameters = std::move(parameters),
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
        if (std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") != nullptr) {
            std::cerr << "Warm Metal preparation: " << preparation.diagnostic << '\n';
        }
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

void resident_gpu_technical_detail_is_complete_or_declines() {
    const auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU denoise was required but no resident Metal session could be prepared"
        );
        return;
    }
    const auto before_denoise = preparation.session->stats();

    std::array<image::AdjustmentNode, 3U> nodes{
        image::AdjustmentNode{
            .node_id = "before-denoise-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.18},
        },
        image::AdjustmentNode{
            .node_id = "technical-denoise",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass = image::DetailEffectsExecutionPass::technical_detail,
                .denoise_luminance = 1.0,
                .denoise_detail = 0.15,
                .denoise_color = 0.90,
            },
        },
        image::AdjustmentNode{
            .node_id = "after-denoise-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.83},
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto rendered = preparation.session->render(nodes, plan, true);
    expect(
        rendered.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && rendered.output.has_value()
            && rendered.output->analyzed_linear.has_value(),
        "a supported technical denoise stage completes entirely on the resident GPU"
    );
    const auto after_denoise = preparation.session->stats();
    expect(
        after_denoise.gpu_buffer_allocation_count
                == before_denoise.gpu_buffer_allocation_count + 2U
            && after_denoise.resident_bytes > before_denoise.resident_bytes,
        "the first GPU denoise render lazily creates only its slot-local intermediate buffers"
    );
    if (rendered.output && rendered.output->analyzed_linear) {
        double mean_delta = 0.0;
        const std::size_t source_stride = source.row_stride_bytes / sizeof(float);
        const auto& output = *rendered.output->analyzed_linear;
        for (std::uint32_t y = 0U; y < source.dimensions.height; ++y) {
            for (std::uint32_t x = 0U; x < source.dimensions.width * 3U; ++x) {
                mean_delta += std::abs(
                    static_cast<double>(source.samples[
                        static_cast<std::size_t>(y) * source_stride + x
                    ]) - output.samples[
                        static_cast<std::size_t>(y) * source.dimensions.width * 3U + x
                    ]
                );
            }
        }
        mean_delta /= static_cast<double>(source.dimensions.pixel_count() * 3U);
        expect(
            mean_delta > 0.01,
            "the GPU denoise stage visibly changes a noisy warm proxy before display output"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).amount = 0.25;
    const auto sharpen_plan = image::compile_edit_execution_plan(nodes);
    const auto sharpened = preparation.session->render(nodes, sharpen_plan, true);
    expect(
        sharpened.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && sharpened.output.has_value()
            && sharpened.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                sharpened.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a mixed technical denoise and capture-sharpening node stays on the resident GPU"
    );
    if (rendered.output && rendered.output->analyzed_linear
        && sharpened.output && sharpened.output->analyzed_linear) {
        double sharpen_delta = 0.0;
        const auto& denoised_pixels = rendered.output->analyzed_linear->samples;
        const auto& sharpened_pixels = sharpened.output->analyzed_linear->samples;
        for (std::size_t index = 0U; index < sharpened_pixels.size(); ++index) {
            sharpen_delta += std::abs(
                static_cast<double>(sharpened_pixels[index]) - denoised_pixels[index]
            );
        }
        sharpen_delta /= static_cast<double>(sharpened_pixels.size());
        expect(
            sharpen_delta > 1.0e-5,
            "the resident capture-sharpening stage visibly changes its denoised input"
        );
    }
    const auto after_sharpen = preparation.session->stats();
    expect(
        after_sharpen.gpu_buffer_allocation_count
                == after_denoise.gpu_buffer_allocation_count + 4U
            && after_sharpen.resident_bytes > after_denoise.resident_bytes,
        "the next execution slot lazily creates its detail raster plus two sharpening scalar buffers"
    );

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).dehaze = 0.25;
    const auto unsupported_plan = image::compile_edit_execution_plan(nodes);
    const auto unsupported = preparation.session->render(nodes, unsupported_plan, false);
    expect(
        unsupported.status
                == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
            && !unsupported.output.has_value()
            && !unsupported.diagnostic.empty(),
        "an unsupported technical-detail combination declines as a whole rather than producing a hybrid frame"
    );
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
            .parameters = image::SharpenAdjustment{
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
            && gpu.output.has_value()
            && gpu.output->analyzed_linear.has_value()
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
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            2.5e-4
        );
        if (!linear_parity) {
            std::cerr << "Texture warm linear parity max="
                      << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the resident Texture path tracks the CPU Oklab-L reference"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).clarity = 0.25;
    const auto unsupported_plan = image::compile_edit_execution_plan(nodes);
    const auto unsupported = preparation.session->render(nodes, unsupported_plan, false);
    expect(
        unsupported.status
                == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
            && !unsupported.output.has_value()
            && !unsupported.diagnostic.empty(),
        "Texture plus unsupported broad Clarity declines as one coherent CPU fallback"
    );
}

void resident_gpu_clarity_is_complete_or_declines() {
    auto source = make_random_image(193U, 113U, true);
    // Clarity is intentionally a preview-scale Metal stage: its large 12px native support is
    // compact at a quarter-scale warm proxy while full-size output keeps using the CPU oracle.
    source.level_zero_to_raster_scale_x = 0.25;
    source.level_zero_to_raster_scale_y = 0.25;
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
            .parameters = image::SharpenAdjustment{
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
            && gpu.output.has_value()
            && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a preview-scale Oklab-L Clarity stage completes on the resident GPU"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            3.0e-4
        );
        if (!linear_parity) {
            std::cerr << "Clarity warm linear parity max="
                      << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the resident Clarity path tracks the CPU Oklab-L reference"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).texture = 0.25;
    const auto combined_plan = image::compile_edit_execution_plan(nodes);
    const auto combined = preparation.session->render(nodes, combined_plan, true);
    expect(
        combined.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && combined.output.has_value()
            && combined.output->analyzed_linear.has_value(),
        "a preview-scale Texture plus Clarity stage completes on the resident GPU"
    );
    if (combined.output && combined.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source, nodes, {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *combined.output->analyzed_linear, cpu.pixels, maximum_error, 3.5e-4
        );
        if (!linear_parity) {
            std::cerr << "Texture + Clarity warm linear parity max="
                      << maximum_error << '\n';
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
            .parameters = image::SharpenAdjustment{
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
            && gpu.output.has_value()
            && gpu.output->analyzed_linear.has_value()
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
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            5.0e-4
        );
        if (!linear_parity) {
            std::cerr << "Local Contrast warm linear parity max="
                      << maximum_error << '\n';
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
        unsupported.status
                == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
            && !unsupported.output.has_value()
            && !unsupported.diagnostic.empty(),
        "Local Contrast plus another neighbourhood band declines as one coherent CPU fallback"
    );
}

void resident_gpu_dehaze_and_defringe_is_complete_or_declines() {
    const auto source = make_random_image(193U, 113U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "GPU dehaze / defringe was required but no resident Metal session could be prepared"
        );
        return;
    }

    std::array<image::AdjustmentNode, 3U> nodes{
        image::AdjustmentNode{
            .node_id = "before-technical-optics-exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.24},
        },
        image::AdjustmentNode{
            .node_id = "technical-dehaze-defringe",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass = image::DetailEffectsExecutionPass::technical_detail,
                .dehaze = 0.56,
                .defringe_purple_amount = 0.41,
                .defringe_purple_hue_low = 272.0,
                .defringe_purple_hue_high = 338.0,
                .defringe_green_amount = 0.35,
                .defringe_green_hue_low = 104.0,
                .defringe_green_hue_high = 162.0,
            },
        },
        image::AdjustmentNode{
            .node_id = "after-technical-optics-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.88},
        },
    };
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto gpu = preparation.session->render(nodes, plan, true);
    expect(
        gpu.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && gpu.output.has_value()
            && gpu.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                gpu.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a supported technical dehaze / defringe stage completes on the resident GPU"
    );
    if (gpu.output && gpu.output->analyzed_linear) {
        const auto cpu = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {.full_dimensions = source.dimensions},
            image::AdjustmentBackendMode::cpu
        );
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            2.5e-4
        );
        if (!linear_parity) {
            std::cerr << "Technical optics warm linear parity max="
                      << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the resident dehaze / defringe path tracks the CPU technical reference"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).denoise_luminance = 0.40;
    const auto unsupported_plan = image::compile_edit_execution_plan(nodes);
    const auto unsupported = preparation.session->render(nodes, unsupported_plan, false);
    expect(
        unsupported.status
                == image::detail::WarmEditGpuSession::RenderStatus::unavailable_or_failed
            && !unsupported.output.has_value()
            && !unsupported.diagnostic.empty(),
        "mixed technical denoise plus dehaze / defringe declines as one CPU fallback"
    );
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

void perceptual_resources_match_cpu_and_have_independent_caches() {
    const auto source = make_random_image(139U, 87U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "perceptual resident Metal was required but could not be prepared"
        );
        return;
    }

    const auto initial = preparation.session->stats();
    expect(
        initial.perceptual_mixer_resource_upload_count == 0U
            && initial.perceptual_range_resource_upload_count == 0U
            && initial.selective_color_resource_upload_count == 0U
            && initial.resource_cache_hit_count == 0U,
        "a new warm session has empty perceptual-color resource caches"
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
            std::cerr << "Perceptual warm render failed: "
                      << gpu.diagnostic << '\n';
            expect(false, description);
            return;
        }
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            2.0e-4
        );
        const auto display_difference =
            rgb8_difference(gpu.output->rgb8, cpu_display.bytes);
        if (!linear_parity || display_difference.maximum > 1U) {
            std::cerr << "Perceptual warm parity: linear max="
                      << maximum_error << ", display max="
                      << static_cast<unsigned int>(display_difference.maximum)
                      << '\n';
        }
        expect(
            linear_parity
                && display_difference.maximum <= 1U
                && display_difference.p99 <= 1U,
            description
        );
    };

    auto nodes = perceptual_nodes();
    render_and_compare(
        nodes,
        "combined perceptual mapping, Point Color, and Selective Color match CPU"
    );
    const auto after_first = preparation.session->stats();
    expect(
        after_first.perceptual_mixer_resource_upload_count
                == initial.perceptual_mixer_resource_upload_count + 1U
            && after_first.perceptual_range_resource_upload_count
                == initial.perceptual_range_resource_upload_count + 1U
            && after_first.selective_color_resource_upload_count
                == initial.selective_color_resource_upload_count + 1U
            && after_first.resource_cache_hit_count
                == initial.resource_cache_hit_count
            && after_first.gpu_buffer_allocation_count
                == initial.gpu_buffer_allocation_count + 3U
            && after_first.resident_bytes > initial.resident_bytes,
        "the first perceptual render publishes one buffer per resource family"
    );

    render_and_compare(nodes, "an identical perceptual render remains CPU-equivalent");
    const auto after_identical = preparation.session->stats();
    expect(
        after_identical.perceptual_mixer_resource_upload_count
                == after_first.perceptual_mixer_resource_upload_count
            && after_identical.perceptual_range_resource_upload_count
                == after_first.perceptual_range_resource_upload_count
            && after_identical.selective_color_resource_upload_count
                == after_first.selective_color_resource_upload_count
            && after_identical.resource_cache_hit_count
                == after_first.resource_cache_hit_count + 3U
            && after_identical.gpu_buffer_allocation_count
                == after_first.gpu_buffer_allocation_count
            && after_identical.resident_bytes == after_first.resident_bytes,
        "an identical perceptual render hits all three immutable caches"
    );

    auto& parameters =
        std::get<image::PerceptualColorAdjustment>(nodes[0U].parameters);
    parameters.vibrance = 0.47;
    parameters.color_range.center_degrees = 31.0;
    parameters.selective_color_relative = true;
    parameters.selective_color_lightness_protection = 0.48;
    render_and_compare(nodes, "changing only perceptual scalars remains CPU-equivalent");
    const auto after_scalars = preparation.session->stats();
    expect(
        after_scalars.perceptual_mixer_resource_upload_count
                == after_identical.perceptual_mixer_resource_upload_count
            && after_scalars.perceptual_range_resource_upload_count
                == after_identical.perceptual_range_resource_upload_count
            && after_scalars.selective_color_resource_upload_count
                == after_identical.selective_color_resource_upload_count
            && after_scalars.resource_cache_hit_count
                == after_identical.resource_cache_hit_count + 3U
            && after_scalars.gpu_buffer_allocation_count
                == after_identical.gpu_buffer_allocation_count,
        "vibrance, primary range, relative mode, and L protection are operation scalars"
    );

    parameters.hue[3U] += 0.04;
    render_and_compare(nodes, "changing one mixer band remains CPU-equivalent");
    const auto after_mixer = preparation.session->stats();
    expect(
        after_mixer.perceptual_mixer_resource_upload_count
                == after_scalars.perceptual_mixer_resource_upload_count + 1U
            && after_mixer.perceptual_range_resource_upload_count
                == after_scalars.perceptual_range_resource_upload_count
            && after_mixer.selective_color_resource_upload_count
                == after_scalars.selective_color_resource_upload_count
            && after_mixer.resource_cache_hit_count
                == after_scalars.resource_cache_hit_count + 2U
            && after_mixer.gpu_buffer_allocation_count
                == after_scalars.gpu_buffer_allocation_count + 1U,
        "changing one color-mixer band uploads only the mixer table"
    );

    parameters.additional_color_ranges[1U].lightness = 0.08;
    render_and_compare(nodes, "changing one ordered Point Color range remains CPU-equivalent");
    const auto after_range = preparation.session->stats();
    expect(
        after_range.perceptual_mixer_resource_upload_count
                == after_mixer.perceptual_mixer_resource_upload_count
            && after_range.perceptual_range_resource_upload_count
                == after_mixer.perceptual_range_resource_upload_count + 1U
            && after_range.selective_color_resource_upload_count
                == after_mixer.selective_color_resource_upload_count
            && after_range.resource_cache_hit_count
                == after_mixer.resource_cache_hit_count + 2U
            && after_range.gpu_buffer_allocation_count
                == after_mixer.gpu_buffer_allocation_count + 1U,
        "changing an additional Point Color range uploads only the range table"
    );

    parameters.selective_color_cmyk[4U][2U] -= 0.05;
    render_and_compare(nodes, "changing one Selective Color value remains CPU-equivalent");
    const auto after_selective = preparation.session->stats();
    expect(
        after_selective.perceptual_mixer_resource_upload_count
                == after_range.perceptual_mixer_resource_upload_count
            && after_selective.perceptual_range_resource_upload_count
                == after_range.perceptual_range_resource_upload_count
            && after_selective.selective_color_resource_upload_count
                == after_range.selective_color_resource_upload_count + 1U
            && after_selective.resource_cache_hit_count
                == after_range.resource_cache_hit_count + 2U
            && after_selective.gpu_buffer_allocation_count
                == after_range.gpu_buffer_allocation_count + 1U,
        "changing the CMYK grid uploads only the Selective Color table"
    );
}

void color_warper_matches_cpu_and_reuses_its_resident_table() {
    const auto source = make_random_image(143U, 89U, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "Color Warper resident Metal was required but could not be prepared"
        );
        return;
    }

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
        const auto gpu = preparation.session->render(nodes, plan, true);
        if (!gpu.output.has_value() || !gpu.output->analyzed_linear.has_value()) {
            std::cerr << "Color Warper warm render failed: "
                      << gpu.diagnostic << '\n';
            expect(false, description);
            return;
        }
        double maximum_error = 0.0;
        const bool linear_parity = linear_close(
            *gpu.output->analyzed_linear,
            cpu.pixels,
            maximum_error,
            2.0e-4
        );
        if (!linear_parity) {
            std::cerr << "Color Warper warm parity: linear max="
                      << maximum_error << '\n';
        }
        expect(linear_parity, description);
    };

    auto nodes = color_warper_nodes();
    const auto initial = preparation.session->stats();
    render_and_compare(nodes, "resident Color Warper matches the CPU lattice oracle");
    const auto after_first = preparation.session->stats();
    expect(
        after_first.perceptual_mixer_resource_upload_count
                == initial.perceptual_mixer_resource_upload_count + 1U
            && after_first.gpu_buffer_allocation_count
                == initial.gpu_buffer_allocation_count + 1U,
        "Color Warper publishes one immutable control lattice in the existing pixel-local table"
    );

    render_and_compare(nodes, "an identical Color Warper reuses its resident lattice");
    const auto after_identical = preparation.session->stats();
    expect(
        after_identical.perceptual_mixer_resource_upload_count
                == after_first.perceptual_mixer_resource_upload_count
            && after_identical.resource_cache_hit_count
                == after_first.resource_cache_hit_count + 1U
            && after_identical.gpu_buffer_allocation_count
                == after_first.gpu_buffer_allocation_count,
        "an unchanged Color Warper control lattice hits its resident resource cache"
    );

    auto& parameters = std::get<image::OklabColorWarperAdjustment>(nodes[0U].parameters);
    parameters.control_points[12U].a_offset += 0.011;
    render_and_compare(nodes, "a changed Color Warper lattice remains CPU-equivalent");
    const auto after_changed = preparation.session->stats();
    expect(
        after_changed.perceptual_mixer_resource_upload_count
                == after_identical.perceptual_mixer_resource_upload_count + 1U
            && after_changed.gpu_buffer_allocation_count
                == after_identical.gpu_buffer_allocation_count + 1U,
        "changing Color Warper geometry uploads only its replacement lattice"
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
    resident_gpu_technical_detail_is_complete_or_declines();
    resident_gpu_texture_is_complete_or_declines();
    resident_gpu_clarity_is_complete_or_declines();
    resident_gpu_local_contrast_is_complete_or_declines();
    resident_gpu_dehaze_and_defringe_is_complete_or_declines();
    advanced_resources_match_cpu_and_reuse_side_table_uploads();
    perceptual_resources_match_cpu_and_have_independent_caches();
    color_warper_matches_cpu_and_reuses_its_resident_table();
    cancellation_is_terminal_without_diagnostic();
    benchmark_resident_backend_when_requested();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

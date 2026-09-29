#include "warm_edit_gpu_contract_cases.hpp"
#include "warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
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

void isolated_denoise_has_progressive_noise_reduction() {
    auto source = make_random_image(96U, 64U, false);
    for (std::size_t pixel = 0; pixel < source.dimensions.pixel_count(); ++pixel) {
        const float base = pixel % 96U < 48U ? 0.18F : 0.55F;
        const float noise = source.samples[pixel * 3U] * 0.04F - 0.042F;
        for (std::size_t channel = 0; channel < 3U; ++channel)
            source.samples[pixel * 3U + channel] = base + noise;
    }
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    if (!preparation.session) {
        expect(std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
               "isolated denoise requires a resident Metal session");
        return;
    }
    const auto noise_rms = [](const image::FloatRgbImage& raster) {
        double square = 0.0;
        for (std::size_t y = 8; y < 56; ++y)
            for (std::size_t x = 8; x < 40; ++x) {
                const double delta = raster.samples[(y * 96U + x) * 3U] - 0.18;
                square += delta * delta;
            }
        return std::sqrt(square / (48.0 * 32.0));
    };
    const double baseline = noise_rms(source);
    double previous_gpu = baseline;
    double previous_cpu = baseline;
    for (const double strength : {0.25, 0.5, 1.0}) {
        const std::array nodes{image::AdjustmentNode{
            .node_id = "isolated-denoise",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass = image::DetailEffectsExecutionPass::technical_detail,
                .denoise_luminance = strength,
                .denoise_detail = 0.5,
            },
        }};
        const auto cpu = image::execute_adjustment_nodes(source, nodes);
        const auto gpu = preparation.session->render(nodes, image::compile_edit_execution_plan(nodes), true);
        expect(gpu.output && gpu.output->analyzed_linear, "isolated GPU denoise publishes pixels");
        if (!gpu.output || !gpu.output->analyzed_linear) return;
        const double cpu_noise = noise_rms(cpu);
        const double gpu_noise = noise_rms(*gpu.output->analyzed_linear);
        std::cout << "Denoise strength=" << strength << " CPU residual=" << cpu_noise / baseline
                  << " Metal residual=" << gpu_noise / baseline << '\n';
        expect(cpu_noise < previous_cpu && gpu_noise < previous_gpu,
               "each denoise strength reduces noise with all other edits neutral");
        expect(std::abs(cpu_noise - gpu_noise) < baseline * 0.10,
               "GPU denoise strength tracks the CPU noise-reduction oracle");
        double edge_contrast = 0.0;
        for (std::size_t y = 8; y < 56; ++y)
            edge_contrast += gpu.output->analyzed_linear->samples[(y * 96U + 48U) * 3U]
                - gpu.output->analyzed_linear->samples[(y * 96U + 47U) * 3U];
        expect(edge_contrast / 48.0 > 0.28,
               "noise reduction preserves the adjacent real luminance edge");
        if (strength == 1.0)
            expect(cpu_noise < baseline * 0.5 && gpu_noise < baseline * 0.5,
                   "maximum denoise removes more than half of flat-field noise RMS");
        previous_cpu = cpu_noise;
        previous_gpu = gpu_noise;
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
            .parameters =
                image::SharpenAdjustment{
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
            && rendered.output.has_value() && rendered.output->analyzed_linear.has_value(),
        "a supported technical denoise stage completes entirely on the resident GPU"
    );
    const auto after_denoise = preparation.session->stats();
    expect(
        after_denoise.gpu_buffer_allocation_count == before_denoise.gpu_buffer_allocation_count + 2U
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
                    static_cast<double>(
                        source.samples[static_cast<std::size_t>(y) * source_stride + x]
                    )
                    - output.samples[static_cast<std::size_t>(y) * source.dimensions.width * 3U + x]
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
            && sharpened.output.has_value() && sharpened.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                sharpened.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "a mixed technical denoise and capture-sharpening node stays on the resident GPU"
    );
    if (rendered.output && rendered.output->analyzed_linear && sharpened.output
        && sharpened.output->analyzed_linear) {
        double sharpen_delta = 0.0;
        const auto& denoised_pixels = rendered.output->analyzed_linear->samples;
        const auto& sharpened_pixels = sharpened.output->analyzed_linear->samples;
        for (std::size_t index = 0U; index < sharpened_pixels.size(); ++index) {
            sharpen_delta +=
                std::abs(static_cast<double>(sharpened_pixels[index]) - denoised_pixels[index]);
        }
        sharpen_delta /= static_cast<double>(sharpened_pixels.size());
        expect(
            sharpen_delta > 1.0e-5,
            "the resident capture-sharpening stage visibly changes its denoised input"
        );
    }
    const auto after_sharpen = preparation.session->stats();
    expect(
        after_sharpen.gpu_buffer_allocation_count == after_denoise.gpu_buffer_allocation_count + 4U
            && after_sharpen.resident_bytes > after_denoise.resident_bytes,
        "the next execution slot lazily creates its detail raster plus two sharpening scalar "
        "buffers"
    );

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).dehaze = 0.25;
    const auto combined_plan = image::compile_edit_execution_plan(nodes);
    const auto combined = preparation.session->render(nodes, combined_plan, true);
    expect(
        combined.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && combined.output.has_value() && combined.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                combined.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "denoise, dehaze and capture sharpening preserve their CPU order in one resident GPU stage"
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
            .parameters =
                image::SharpenAdjustment{
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
            && gpu.output.has_value() && gpu.output->analyzed_linear.has_value()
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
        const bool linear_parity =
            linear_close(*gpu.output->analyzed_linear, cpu.pixels, maximum_error, 2.5e-4);
        if (!linear_parity) {
            std::cerr << "Technical optics warm linear parity max=" << maximum_error << '\n';
        }
        expect(
            linear_parity,
            "the resident dehaze / defringe path tracks the CPU technical reference"
        );
    }

    std::get<image::SharpenAdjustment>(nodes[1U].parameters).denoise_luminance = 0.40;
    const auto combined_plan = image::compile_edit_execution_plan(nodes);
    const auto combined = preparation.session->render(nodes, combined_plan, true);
    expect(
        combined.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && combined.output.has_value() && combined.output->analyzed_linear.has_value()
            && std::ranges::all_of(
                combined.output->analyzed_linear->samples,
                [](const float value) { return std::isfinite(value); }
            ),
        "mixed technical denoise plus dehaze / defringe completes as one ordered GPU stage"
    );
}

} // namespace

int run_resident_gpu_technical_detail_contract() {
    failures = 0;
    isolated_denoise_has_progressive_noise_reduction();
    resident_gpu_technical_detail_is_complete_or_declines();
    return failures;
}

int run_resident_gpu_dehaze_and_defringe_contract() {
    failures = 0;
    resident_gpu_dehaze_and_defringe_is_complete_or_declines();
    return failures;
}

} // namespace shadow::image::warm_edit_gpu_contract

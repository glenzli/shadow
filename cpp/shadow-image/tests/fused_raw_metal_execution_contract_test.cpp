#include "contract_test_assertions.hpp"
#include "fused_raw_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include "../src/raw/metal_raw_development.hpp"
#include "../src/raw/raw_denoise_plan.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::ScopedEnvironment;

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] image::DcpColorTransform
fused_dcp_transform(const image::RawFrameDescriptor& descriptor) {
    image::DcpProfile profile;
    profile.unique_camera_model = "FUSED RAW CONTRACT";
    profile.profile_name = "Fused RAW Contract";
    profile.calibration1.illuminant = 21U;
    profile.calibration1.illuminant_was_explicit = true;
    profile.calibration1.color_matrix = image::DcpMatrix3x3{{
        1.0 / 0.95047,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0 / 1.08883,
    }};
    profile.calibration1.forward_matrix = image::DcpMatrix3x3{{
        0.964295676,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.825104603,
    }};
    profile.calibration1.hue_sat_map = image::DcpHsvTable{
        .hue_divisions = 1U,
        .saturation_divisions = 2U,
        .value_divisions = 1U,
        .encoding = image::DcpTableEncoding::linear,
        .entries = {
            image::DcpHsvDelta{},
            image::DcpHsvDelta{.value_scale = 0.82F},
        },
    };
    profile.look_table = image::DcpHsvTable{
        .hue_divisions = 1U,
        .saturation_divisions = 2U,
        .value_divisions = 1U,
        .encoding = image::DcpTableEncoding::linear,
        .entries = {
            image::DcpHsvDelta{},
            image::DcpHsvDelta{.saturation_scale = 0.91F, .value_scale = 0.74F},
        },
    };
    profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.34F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    return image::compile_dcp_color_transform(
        image::CameraProfileDefinition{
            .profile = std::move(profile),
            .normalized_camera_model = "FUSED RAW CONTRACT",
            .content_identity = "sha256:fused-raw-contract",
            .source_name = "fused-raw-contract.dcp",
        },
        descriptor
    );
}

[[nodiscard]] image::RawFrame benchmark_frame(const image::Dimensions dimensions) {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "fused-benchmark";
    descriptor.provider_version = "v1";
    descriptor.storage_dimensions = dimensions;
    descriptor.active_dimensions = dimensions;
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.cfa_pattern = "RGGB";
    descriptor.bits_per_sample = 14U;
    descriptor.black_levels = {256U, 256U, 256U, 256U};
    descriptor.white_levels = {16'383U, 16'383U, 16'383U, 16'383U};
    descriptor.as_shot_neutral = {0.52, 1.0, 1.0, 0.41};
    frame.samples.resize(static_cast<std::size_t>(dimensions.pixel_count()));
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto signal = static_cast<std::uint16_t>(
                512U
                + (static_cast<std::uint64_t>(x) * 37U + static_cast<std::uint64_t>(y) * 53U)
                      % 14'000U
            );
            frame.samples[static_cast<std::size_t>(y) * dimensions.width + x] = signal;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame chromatic_edge_frame(const std::int32_t orientation) {
    auto frame = synthetic_frame(orientation);
    const auto width = frame.descriptor.storage_dimensions.width;
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const bool bright = x >= width / 2U;
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t signal =
                colour == image::RawCfaColor::green ? (bright ? 900U : 120U)
                : colour == image::RawCfaColor::red ? (bright ? 850U : 60U)
                                                    : (bright ? 100U : 880U);
            frame.samples[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + signal);
        }
    }
    return frame;
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

void metal_full_resolution_stays_within_the_linear_u16_contract() {
    expect(
        image::raw_development_backend_identity(image::RawDevelopmentBackend::cpu)
            != image::raw_development_backend_identity(image::RawDevelopmentBackend::metal),
        "CPU and Metal development identities remain distinct"
    );
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for this validation run but no Metal backend is available"
        );
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31,
        -0.27,
        0.08,
        -0.06,
        1.14,
        -0.03,
        0.04,
        -0.22,
        1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu
        );
        const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        expect(metal.valid(), "Metal full result has a complete typed contract");
        expect(
            metal.backend == image::RawDevelopmentBackend::metal,
            "forced Metal result records its effective backend"
        );
        expect(
            metal.scene_linear.dimensions == cpu.scene_linear.dimensions
                && metal.scene_linear.samples.size() == cpu.scene_linear.samples.size(),
            "Metal preserves CPU dimensions and packed sample count"
        );
        expect(
            metal.scene_linear.samples == repeated.scene_linear.samples,
            "repeated Metal development is byte deterministic"
        );

        std::vector<float> differences;
        differences.reserve(cpu.scene_linear.samples.size());
        double total_difference = 0.0;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
            const float difference =
                std::abs(cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]);
            differences.push_back(difference);
            total_difference += difference;
        }
        std::sort(differences.begin(), differences.end());
        const auto p99_index = differences.empty() ? 0U : (differences.size() - 1U) * 99U / 100U;
        const auto maximum = differences.empty() ? 0U : differences.back();
        const auto p99 = differences.empty() ? 0U : differences[p99_index];
        const double mean = differences.empty() ? 0.0
                                                : static_cast<double>(total_difference)
                                                      / static_cast<double>(differences.size());
        expect(
            maximum <= 4.0e-5F,
            "Metal maximum error stays within fp32 reconstruction tolerance"
        );
        expect(p99 <= 2.0e-5F, "Metal p99 error stays within fp32 reconstruction tolerance");
        expect(mean <= 1.0e-6, "Metal mean error stays within fp32 reconstruction tolerance");
    }
}

void metal_area_preview_preserves_the_cfa_footprint_contract() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for area-preview validation but no Metal backend is available"
        );
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31,
        -0.27,
        0.08,
        -0.06,
        1.14,
        -0.03,
        0.04,
        -0.22,
        1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        for (const bool terminal_boundary : {false, true}) {
            auto frame = synthetic_frame(orientation);
            if (terminal_boundary) {
                const auto& descriptor = frame.descriptor;
                const auto first_x = descriptor.active_margins.left;
                const auto first_y = descriptor.active_margins.top;
                const auto terminal_x = first_x + descriptor.active_dimensions.width / 2U;
                for (std::uint32_t y = first_y; y < first_y + descriptor.active_dimensions.height;
                     ++y) {
                    for (std::uint32_t x = terminal_x;
                         x < first_x + descriptor.active_dimensions.width;
                         ++x) {
                        const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
                        const auto sample =
                            static_cast<std::size_t>(y) * descriptor.storage_dimensions.width + x;
                        frame.samples[sample] =
                            static_cast<std::uint16_t>(descriptor.white_levels[site]);
                    }
                }
            }
            const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
                frame,
                transform,
                3U,
                image::RawDevelopmentBackendMode::cpu
            );
            const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
                frame,
                transform,
                3U,
                image::RawDevelopmentBackendMode::metal
            );
            const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
                frame,
                transform,
                3U,
                image::RawDevelopmentBackendMode::metal
            );
            expect(
                metal.valid() && metal.backend == image::RawDevelopmentBackend::metal
                    && metal.demosaic_receipt.algorithm
                           == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
                "Metal area preview retains the typed CFA-footprint receipt"
            );
            expect(
                metal.scene_linear.dimensions == cpu.scene_linear.dimensions
                    && metal.scene_linear.samples.size() == cpu.scene_linear.samples.size(),
                "Metal area preview preserves CPU output dimensions and packing"
            );
            expect(
                metal.scene_linear.samples == repeated.scene_linear.samples,
                "Metal area preview is byte deterministic"
            );
            float maximum_error = 0.0F;
            double total_error = 0.0;
            for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
                const float error =
                    std::abs(cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]);
                maximum_error = std::max(maximum_error, error);
                total_error += error;
            }
            const double mean_error =
                cpu.scene_linear.samples.empty()
                    ? 0.0
                    : static_cast<double>(total_error)
                          / static_cast<double>(cpu.scene_linear.samples.size());
            expect(maximum_error <= 4.0e-5F, "Metal area preview stays within fp32 CPU tolerance");
            expect(mean_error <= 1.0e-5, "Metal area preview stays within fp32 mean tolerance");
        }
    }
}

void metal_high_quality_matches_edge_aware_cpu_reconstruction() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31,
        -0.27,
        0.08,
        -0.06,
        1.14,
        -0.03,
        0.04,
        -0.22,
        1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = chromatic_edge_frame(orientation);
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::high
        );
        const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::high
        );
        const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::high
        );
        const auto balanced = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        expect(
            metal.valid()
                && metal.demosaic_receipt.algorithm
                       == image::RawDemosaicAlgorithm::bayer_edge_aware_v1
                && metal.scene_linear.samples == repeated.scene_linear.samples,
            "Metal high-quality RAW reconstruction is explicit and byte deterministic"
        );
        expect(
            metal.scene_linear.samples != balanced.scene_linear.samples,
            "Metal high quality is not aliased to balanced bilinear reconstruction"
        );
        float maximum_error = 0.0F;
        double total_error = 0.0;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
            const float error =
                std::abs(cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]);
            maximum_error = std::max(maximum_error, error);
            total_error += error;
        }
        const double mean_error =
            cpu.scene_linear.samples.empty()
                ? 0.0
                : total_error / static_cast<double>(cpu.scene_linear.samples.size());
        expect(
            maximum_error <= 8.0e-5F,
            "Metal edge-aware maximum error stays within the fp32 CPU tolerance"
        );
        expect(
            mean_error <= 1.0e-5,
            "Metal edge-aware mean error stays within the fp32 CPU tolerance"
        );

        const auto preview = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::metal,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::high
        );
        expect(
            preview.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
            "Metal high-quality requests retain CFA-area preview semantics"
        );
    }
}

void metal_reconstruction_and_dcp_share_one_tiled_transaction() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }
    const auto frame = synthetic_frame(5);
    const auto dcp = fused_dcp_transform(frame.descriptor);
    const image::RawFrameLinearTransform transform{dcp.camera_to_linear_srgb_d65};

    auto staged = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        transform,
        std::nullopt,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced
    );
    expect(
        staged.development.has_value() && !staged.dcp_applied,
        "the reconstruction-only control remains a valid Metal stage"
    );
    if (!staged.development.has_value()) {
        return;
    }
    {
        const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
        expect(
            image::apply_dcp_color_rendering_stages(staged.development->scene_linear, dcp)
                == image::DcpColorExecutionBackend::metal,
            "the staged control executes DCP on Metal"
        );
    }

    const auto fused = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        transform,
        std::nullopt,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced,
        image::detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = &dcp,
        }
    );
    const auto repeated = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        transform,
        std::nullopt,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced,
        image::detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = &dcp,
        }
    );
    expect(
        fused.development.has_value() && fused.dcp_applied && repeated.development.has_value()
            && repeated.dcp_applied,
        "DCP is encoded into each resident RAW output tile before its only readback"
    );
    if (!fused.development.has_value() || !repeated.development.has_value()) {
        return;
    }
    expect(
        fused.development->scene_linear.samples == staged.development->scene_linear.samples,
        "fused RAW/DCP pixels are byte-identical to the same two Metal kernels staged separately"
    );
    expect(
        fused.development->scene_linear.samples == repeated.development->scene_linear.samples,
        "fused RAW/DCP execution remains byte deterministic"
    );
}

void metal_denoise_reconstruction_and_dcp_share_one_resident_transaction() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }
    const auto frame = synthetic_frame(5);
    const auto dcp = fused_dcp_transform(frame.descriptor);
    const image::RawFrameLinearTransform transform{dcp.camera_to_linear_srgb_d65};
    const auto denoise = image::detail::prepare_raw_bayer_denoise(
        frame,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::noise_robust,
            .iso_sensitivity = 1'600.0,
            .preview = false,
        }
    );
    auto staged_frame = frame;
    const auto staged_denoise = image::detail::try_denoise_bayer_raw_frame_metal(
        staged_frame,
        denoise.mode,
        denoise.iso_sensitivity
    );
    auto staged = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        staged_frame,
        transform,
        std::nullopt,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced,
        image::detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = &dcp,
        }
    );
    const auto fused = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        transform,
        std::nullopt,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced,
        image::detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = &dcp,
            .raw_denoise = &denoise,
        }
    );
    const auto repeated = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        transform,
        std::nullopt,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced,
        image::detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = &dcp,
            .raw_denoise = &denoise,
        }
    );
    expect(
        staged_denoise.applied && staged.development.has_value() && staged.dcp_applied
            && !staged.raw_denoise_applied,
        "the staged control materializes Metal denoise before its RAW/DCP transaction"
    );
    expect(
        fused.development.has_value() && fused.raw_denoise_applied && fused.dcp_applied
            && repeated.development.has_value() && repeated.raw_denoise_applied
            && repeated.dcp_applied,
        "same-CFA denoise remains resident through RAW reconstruction and DCP rendering"
    );
    if (!staged.development.has_value() || !fused.development.has_value()
        || !repeated.development.has_value()) {
        return;
    }
    expect(
        fused.development->scene_linear.samples == staged.development->scene_linear.samples,
        "resident RAW denoise is byte-identical to the same Metal kernels staged separately"
    );
    expect(
        fused.development->scene_linear.samples == repeated.development->scene_linear.samples,
        "resident RAW sensor development remains byte deterministic"
    );
}

void metal_sensor_clipping_projection_matches_the_cpu_contract() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        auto frame = synthetic_frame(orientation);
        const auto width = frame.descriptor.storage_dimensions.width;
        const auto left = frame.descriptor.active_margins.left;
        const auto top = frame.descriptor.active_margins.top;
        const auto black_site = static_cast<std::size_t>((top & 1U) * 2U + (left & 1U));
        const auto white_x = left + 1U;
        const auto white_y = top + 1U;
        const auto white_site = static_cast<std::size_t>((white_y & 1U) * 2U + (white_x & 1U));
        frame.samples[static_cast<std::size_t>(top) * width + left] =
            static_cast<std::uint16_t>(frame.descriptor.black_levels[black_site]);
        frame.samples[static_cast<std::size_t>(white_y) * width + white_x] =
            static_cast<std::uint16_t>(frame.descriptor.white_levels[white_site]);

        for (const bool preview : {false, true}) {
            const std::optional<std::uint32_t> max_edge =
                preview ? std::optional<std::uint32_t>{3U} : std::nullopt;
            const auto reconstruction =
                max_edge.has_value()
                    ? image::proxy_dimensions(frame.descriptor.active_dimensions, *max_edge)
                    : frame.descriptor.active_dimensions;
            const image::Dimensions target =
                orientation == 5 || orientation == 6
                    ? image::Dimensions{reconstruction.height, reconstruction.width}
                    : reconstruction;
            const auto cpu = image::project_sensor_clipping_mask(frame, target);
            const auto metal = image::detail::try_develop_bayer_linear_srgb_f32_metal(
                frame,
                transform,
                max_edge,
                image::RawHighlightRecoveryIntent::provider_default,
                image::RawDevelopmentQuality::balanced,
                image::detail::MetalRawDevelopmentContinuations{
                    .project_sensor_clipping = true,
                }
            );
            expect(
                metal.development.has_value() && metal.sensor_clipping_mask.has_value(),
                "Metal RAW development returns its requested clipping diagnostic"
            );
            if (!metal.sensor_clipping_mask.has_value()) {
                continue;
            }
            expect(
                metal.sensor_clipping_mask->dimensions == cpu.dimensions
                    && metal.sensor_clipping_mask->samples == cpu.samples
                    && metal.sensor_clipping_mask->highlight_pixel_count
                           == cpu.highlight_pixel_count
                    && metal.sensor_clipping_mask->shadow_pixel_count == cpu.shadow_pixel_count,
                "Metal clipping projection exactly matches CPU orientation and target-bin semantics"
            );
        }
    }
}

void benchmark_fused_raw_dcp_when_requested() {
    if (!environment_enabled("SHADOW_TEST_FUSED_RAW_DCP_BENCHMARK")) {
        return;
    }
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        throw std::runtime_error("Metal is unavailable for the fused RAW/DCP benchmark");
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto frame = benchmark_frame(dimensions);
    const auto dcp = fused_dcp_transform(frame.descriptor);
    const image::RawFrameLinearTransform transform{dcp.camera_to_linear_srgb_d65};
    std::uint64_t checksum = 0U;
    constexpr std::size_t iterations = 3U;
    const double staged = median_milliseconds(iterations, [&]() {
        auto attempt = image::detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            std::nullopt,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::balanced
        );
        if (!attempt.development.has_value()) {
            throw std::runtime_error(attempt.diagnostic);
        }
        {
            const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
            static_cast<void>(
                image::apply_dcp_color_rendering_stages(attempt.development->scene_linear, dcp)
            );
        }
        checksum += static_cast<std::uint64_t>(
            attempt.development->scene_linear
                .samples[attempt.development->scene_linear.samples.size() / 2U]
            * 1'000.0F
        );
    });
    const double fused = median_milliseconds(iterations, [&]() {
        auto attempt = image::detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            std::nullopt,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::balanced,
            image::detail::MetalRawDevelopmentContinuations{
                .dcp_color_transform = &dcp,
            }
        );
        if (!attempt.development.has_value() || !attempt.dcp_applied) {
            throw std::runtime_error(attempt.diagnostic);
        }
        checksum += static_cast<std::uint64_t>(
            attempt.development->scene_linear
                .samples[attempt.development->scene_linear.samples.size() / 2U]
            * 1'000.0F
        );
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH fused RAW+DCP " << dimensions.width
              << 'x' << dimensions.height << " staged-Metal=" << staged << "ms"
              << " fused-Metal=" << fused << "ms"
              << " speedup=" << staged / fused << 'x' << " checksum=" << checksum << '\n';
}

void benchmark_fused_raw_sensor_development_when_requested() {
    if (!environment_enabled("SHADOW_TEST_FUSED_RAW_SENSOR_BENCHMARK")) {
        return;
    }
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        throw std::runtime_error("Metal is unavailable for the fused RAW sensor benchmark");
    }
    constexpr image::Dimensions dimensions{3'000U, 2'000U};
    const auto frame = benchmark_frame(dimensions);
    const auto dcp = fused_dcp_transform(frame.descriptor);
    const image::RawFrameLinearTransform transform{dcp.camera_to_linear_srgb_d65};
    const auto denoise = image::detail::prepare_raw_bayer_denoise(
        frame,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::noise_robust,
            .iso_sensitivity = 1'600.0,
            .preview = false,
        }
    );
    std::uint64_t checksum = 0U;
    constexpr std::size_t iterations = 3U;
    const double staged = median_milliseconds(iterations, [&]() {
        auto staged_frame = frame;
        const auto denoise_attempt = image::detail::try_denoise_bayer_raw_frame_metal(
            staged_frame,
            denoise.mode,
            denoise.iso_sensitivity
        );
        auto attempt = image::detail::try_develop_bayer_linear_srgb_f32_metal(
            staged_frame,
            transform,
            std::nullopt,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::balanced,
            image::detail::MetalRawDevelopmentContinuations{
                .dcp_color_transform = &dcp,
            }
        );
        if (!denoise_attempt.applied || !attempt.development.has_value() || !attempt.dcp_applied) {
            throw std::runtime_error(
                denoise_attempt.diagnostic.empty() ? attempt.diagnostic : denoise_attempt.diagnostic
            );
        }
        checksum += static_cast<std::uint64_t>(
            attempt.development->scene_linear
                .samples[attempt.development->scene_linear.samples.size() / 2U]
            * 1'000.0F
        );
    });
    const double fused = median_milliseconds(iterations, [&]() {
        auto attempt = image::detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            std::nullopt,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::balanced,
            image::detail::MetalRawDevelopmentContinuations{
                .dcp_color_transform = &dcp,
                .raw_denoise = &denoise,
            }
        );
        if (!attempt.development.has_value() || !attempt.raw_denoise_applied
            || !attempt.dcp_applied) {
            throw std::runtime_error(attempt.diagnostic);
        }
        checksum += static_cast<std::uint64_t>(
            attempt.development->scene_linear
                .samples[attempt.development->scene_linear.samples.size() / 2U]
            * 1'000.0F
        );
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH fused RAW sensor " << dimensions.width
              << 'x' << dimensions.height << " staged-Metal=" << staged << "ms"
              << " fused-Metal=" << fused << "ms"
              << " speedup=" << staged / fused << 'x' << " checksum=" << checksum << '\n';
}

void benchmark_fused_sensor_clipping_when_requested() {
    if (!environment_enabled("SHADOW_TEST_FUSED_SENSOR_CLIPPING_BENCHMARK")) {
        return;
    }
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        throw std::runtime_error("Metal is unavailable for the fused clipping benchmark");
    }
    constexpr image::Dimensions dimensions{3'000U, 2'000U};
    const auto frame = benchmark_frame(dimensions);
    const image::RawFrameLinearTransform transform{{
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    }};
    std::uint64_t checksum = 0U;
    constexpr std::size_t iterations = 3U;
    const double staged = median_milliseconds(iterations, [&]() {
        const auto clipping = image::project_sensor_clipping_mask(frame, dimensions);
        auto attempt = image::detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            std::nullopt,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::balanced
        );
        if (!attempt.development.has_value()) {
            throw std::runtime_error(attempt.diagnostic);
        }
        checksum += clipping.highlight_pixel_count + clipping.shadow_pixel_count;
        checksum += static_cast<std::uint64_t>(
            attempt.development->scene_linear
                .samples[attempt.development->scene_linear.samples.size() / 2U]
            * 1'000.0F
        );
    });
    const double fused = median_milliseconds(iterations, [&]() {
        auto attempt = image::detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            std::nullopt,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::balanced,
            image::detail::MetalRawDevelopmentContinuations{
                .project_sensor_clipping = true,
            }
        );
        if (!attempt.development.has_value() || !attempt.sensor_clipping_mask.has_value()) {
            throw std::runtime_error(attempt.diagnostic);
        }
        checksum += attempt.sensor_clipping_mask->highlight_pixel_count
                    + attempt.sensor_clipping_mask->shadow_pixel_count;
        checksum += static_cast<std::uint64_t>(
            attempt.development->scene_linear
                .samples[attempt.development->scene_linear.samples.size() / 2U]
            * 1'000.0F
        );
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH fused sensor clipping "
              << dimensions.width << 'x' << dimensions.height << " staged=" << staged << "ms"
              << " fused=" << fused << "ms"
              << " speedup=" << staged / fused << 'x' << " checksum=" << checksum << '\n';
}

void benchmark_edge_aware_metal_when_requested() {
    if (!environment_enabled("SHADOW_TEST_EDGE_AWARE_METAL_BENCHMARK")) {
        return;
    }
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        throw std::runtime_error("Metal is unavailable for the edge-aware benchmark");
    }
    constexpr image::Dimensions dimensions{1'200U, 800U};
    const auto frame = benchmark_frame(dimensions);
    const image::RawFrameLinearTransform transform{{
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    }};
    std::uint64_t checksum = 0U;
    constexpr std::size_t iterations = 3U;
    const double cpu = median_milliseconds(iterations, [&]() {
        const auto developed = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::high
        );
        checksum += static_cast<std::uint64_t>(
            developed.scene_linear.samples[developed.scene_linear.samples.size() / 2U] * 1'000.0F
        );
    });
    const double metal = median_milliseconds(iterations, [&]() {
        const auto developed = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal,
            image::RawHighlightRecoveryIntent::provider_default,
            image::RawDevelopmentQuality::high
        );
        checksum += static_cast<std::uint64_t>(
            developed.scene_linear.samples[developed.scene_linear.samples.size() / 2U] * 1'000.0F
        );
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH edge-aware RAW " << dimensions.width
              << 'x' << dimensions.height << " CPU=" << cpu << "ms"
              << " Metal=" << metal << "ms"
              << " speedup=" << cpu / metal << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int main() {
    metal_full_resolution_stays_within_the_linear_u16_contract();
    metal_area_preview_preserves_the_cfa_footprint_contract();
    metal_high_quality_matches_edge_aware_cpu_reconstruction();
    metal_reconstruction_and_dcp_share_one_tiled_transaction();
    metal_denoise_reconstruction_and_dcp_share_one_resident_transaction();
    metal_sensor_clipping_projection_matches_the_cpu_contract();
    benchmark_fused_raw_dcp_when_requested();
    benchmark_fused_raw_sensor_development_when_requested();
    benchmark_fused_sensor_clipping_when_requested();
    benchmark_edge_aware_metal_when_requested();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

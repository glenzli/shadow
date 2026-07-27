#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_denoise.hpp>

#include "bayer_sampling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

inline constexpr std::string_view raw_pipeline_environment = "SHADOW_RAW_PIPELINE";
inline constexpr std::string_view raw_frame_pipeline_identity =
    "shadow-raw-frame-developer-v1:bayer-area-preview+bayer-bilinear:"
    "raw-denoise-cfa-bilateral-v1:as-shot-neutral:camera-matrix:scene-linear-f32";
inline constexpr std::uint32_t source_luminance_sample_edge = 256U;
inline constexpr double source_luminance_percentile = 0.990;

[[nodiscard]] const char* path_name(const RawPipelinePath path) noexcept {
    switch (path) {
    case RawPipelinePath::decoded_raster:
        return "decoded-raster";
    case RawPipelinePath::shadow_raw_frame:
        return "shadow-raw-frame";
    case RawPipelinePath::provider_processed_compatibility:
        return "provider-processed-compatibility";
    }
    return "unknown";
}

[[nodiscard]] const char* mode_name(const RawPipelineMode mode) noexcept {
    switch (mode) {
    case RawPipelineMode::automatic:
        return "auto";
    case RawPipelineMode::require_shadow_raw_frame:
        return "raw-frame";
    case RawPipelineMode::require_provider_processed:
        return "processed";
    }
    return "unknown";
}

[[nodiscard]] const char* camera_profile_status_name(
    const RawCameraProfileStatus status
) noexcept {
    switch (status) {
    case RawCameraProfileStatus::not_considered:
        return "not-considered";
    case RawCameraProfileStatus::no_match:
        return "no-match";
    case RawCameraProfileStatus::applied:
        return "applied";
    case RawCameraProfileStatus::matched_not_applied:
        return "matched-not-applied";
    }
    return "unknown";
}

[[nodiscard]] bool supported_orientation(const std::int32_t orientation) noexcept {
    return orientation == 0 || orientation == 3 || orientation == 5 || orientation == 6;
}

[[nodiscard]] Dimensions oriented_dimensions(
    const Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    if (orientation == 5 || orientation == 6) {
        return Dimensions{dimensions.height, dimensions.width};
    }
    return dimensions;
}

[[nodiscard]] std::array<double, 3U> canonical_camera_neutral(
    const RawFrameDescriptor& descriptor
) {
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    for (std::size_t site = 0U; site < descriptor.bayer_2x2.size(); ++site) {
        std::size_t channel = 0U;
        switch (descriptor.bayer_2x2[site]) {
        case RawCfaColor::red:
            channel = 0U;
            break;
        case RawCfaColor::green:
            channel = 1U;
            break;
        case RawCfaColor::blue:
            channel = 2U;
            break;
        case RawCfaColor::unknown:
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame has an unknown CFA colour in its neutral calibration"
            );
        }
        const double neutral = descriptor.as_shot_neutral[site];
        if (!std::isfinite(neutral) || neutral <= 0.0) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame does not provide a usable as-shot camera neutral"
            );
        }
        totals[channel] += neutral;
        ++counts[channel];
    }
    std::array<double, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        if (counts[channel] == 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame camera neutral does not cover RGB"
            );
        }
        result[channel] = totals[channel] / static_cast<double>(counts[channel]);
    }
    return result;
}

[[nodiscard]] std::array<double, 3U> white_balance_multipliers(
    const RawFrameDescriptor& descriptor
) {
    const auto neutral = canonical_camera_neutral(descriptor);
    std::array<double, 3U> multipliers{
        1.0 / neutral[0],
        1.0 / neutral[1],
        1.0 / neutral[2],
    };
    const double green = multipliers[1];
    if (!std::isfinite(green) || green <= 0.0) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "RAW frame camera neutral produces an invalid white balance"
        );
    }
    for (auto& value : multipliers) {
        value /= green;
    }
    return multipliers;
}

using Matrix3 = std::array<double, 9U>;

[[nodiscard]] Matrix3 multiply_matrix(
    const Matrix3& left,
    const Matrix3& right
) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] +=
                    left[row * 3U + inner] * right[inner * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] RawFrameLinearTransform generic_raw_frame_transform(
    const RawFrameDescriptor& descriptor
) {
    Matrix3 camera_to_srgb{};
    if (descriptor.has_camera_to_linear_srgb_d65) {
        camera_to_srgb = descriptor.camera_to_linear_srgb_d65;
    } else {
        if (!descriptor.has_camera_to_xyz_d50) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "RAW frame does not provide a camera colour calibration"
            );
        }
        // RawFrame's D50 matrix is input-major for compatibility with the original provider ABI:
        // XYZ[j] = sum(camera[i] * matrix[i * 3 + j]). Transpose it once into the
        // output-major convention consumed by the fused renderer.
        Matrix3 camera_to_xyz_d50{};
        for (std::size_t input = 0U; input < 3U; ++input) {
            for (std::size_t output = 0U; output < 3U; ++output) {
                camera_to_xyz_d50[output * 3U + input] =
                    descriptor.camera_to_xyz_d50[input * 3U + output];
            }
        }
        constexpr Matrix3 d50_to_d65{
            0.9555766, -0.0230393, 0.0631636,
            -0.0282895, 1.0099416, 0.0210077,
            0.0122982, -0.0204830, 1.3299098,
        };
        constexpr Matrix3 xyz_d65_to_srgb{
            3.2404542, -1.5371385, -0.4985314,
            -0.9692660, 1.8760108, 0.0415560,
            0.0556434, -0.2040259, 1.0572252,
        };
        camera_to_srgb = multiply_matrix(
            xyz_d65_to_srgb,
            multiply_matrix(d50_to_d65, camera_to_xyz_d50)
        );
    }

    const auto white_balance = white_balance_multipliers(descriptor);
    // Fold WB into the input columns so the hot loop performs one matrix multiply.
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            camera_to_srgb[output * 3U + input] *= white_balance[input];
        }
    }
    return RawFrameLinearTransform{camera_to_srgb};
}

[[nodiscard]] double sampled_scene_linear_luminance_percentile(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const DcpColorTransform* camera_profile
) {
    detail::validate_bayer_frame(frame, "RAW source luminance sampling");
    if (!transform.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW source luminance sampling requires a finite camera transform"
        );
    }

    const Dimensions active = frame.descriptor.active_dimensions;
    const std::uint32_t sample_columns = std::min(active.width, source_luminance_sample_edge);
    const std::uint32_t sample_rows = std::min(active.height, source_luminance_sample_edge);
    SceneLinearRgbFrame samples;
    samples.dimensions = {sample_columns, sample_rows};
    samples.row_stride_bytes = static_cast<std::size_t>(sample_columns) * 3U * sizeof(float);
    samples.samples.resize(
        static_cast<std::size_t>(sample_columns) * sample_rows * 3U
    );
    const auto source_coordinate = [](const std::uint32_t index,
                                      const std::uint32_t sample_count,
                                      const std::uint32_t full_count) noexcept {
        if (sample_count <= 1U) {
            return full_count / 2U;
        }
        return static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(index) * (full_count - 1U) / (sample_count - 1U)
        );
    };
    for (std::uint32_t y = 0U; y < sample_rows; ++y) {
        const std::uint32_t raw_y = frame.descriptor.active_margins.top
            + source_coordinate(y, sample_rows, active.height);
        for (std::uint32_t x = 0U; x < sample_columns; ++x) {
            const std::uint32_t raw_x = frame.descriptor.active_margins.left
                + source_coordinate(x, sample_columns, active.width);
            const auto camera = detail::bilinear_camera_rgb_at(frame, raw_x, raw_y);
            const std::size_t index =
                (static_cast<std::size_t>(y) * sample_columns + x) * 3U;
            for (std::size_t output = 0U; output < 3U; ++output) {
                double linear_srgb = 0.0;
                for (std::size_t input = 0U; input < 3U; ++input) {
                    linear_srgb += transform.camera_to_linear_srgb_d65[output * 3U + input]
                        * static_cast<double>(camera[input]);
                }
                samples.samples[index + output] = static_cast<float>(linear_srgb);
            }
        }
    }
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        static_cast<void>(apply_dcp_color_rendering_stages(samples, *camera_profile));
    }

    std::vector<double> luminances;
    luminances.reserve(samples.dimensions.pixel_count());
    for (std::size_t index = 0U; index < samples.samples.size(); index += 3U) {
        const double luminance = static_cast<double>(samples.samples[index]) * 0.2126
            + static_cast<double>(samples.samples[index + 1U]) * 0.7152
            + static_cast<double>(samples.samples[index + 2U]) * 0.0722;
        if (std::isfinite(luminance) && luminance >= 0.0) {
            luminances.push_back(luminance);
        }
    }
    if (luminances.empty()) {
        return 0.0;
    }
    const std::size_t percentile_index = std::min(
        luminances.size() - 1U,
        static_cast<std::size_t>(std::floor(
            static_cast<double>(luminances.size() - 1U) * source_luminance_percentile
        ))
    );
    std::nth_element(
        luminances.begin(),
        luminances.begin() + static_cast<std::ptrdiff_t>(percentile_index),
        luminances.end()
    );
    return luminances[percentile_index];
}

[[nodiscard]] RawDevelopmentReceipt raw_frame_development_receipt(
    const RawFrameDescriptor& descriptor,
    const RawDevelopmentPlan& plan,
    const Dimensions rendered_dimensions,
    const RawDemosaicReceipt& demosaic,
    const RawDevelopmentBackend backend,
    const DcpColorTransform* camera_profile,
    const RawBayerDenoiseReceipt& raw_denoise
) {
    RawDevelopmentReceipt receipt;
    receipt.schema_version = raw_development_receipt_schema_version;
    receipt.provider_id = descriptor.provider_id.empty()
        ? "provider-neutral-raw-frame" : descriptor.provider_id;
    receipt.provider_version = descriptor.provider_version.empty()
        ? "unrecorded" : descriptor.provider_version;
    switch (demosaic.algorithm) {
    case RawDemosaicAlgorithm::bayer_area_preview_v1:
        receipt.development_settings_signature = "shadow-raw-v1;demosaic=bayer-area-preview";
        break;
    case RawDemosaicAlgorithm::bayer_edge_aware_v1:
        receipt.development_settings_signature = "shadow-raw-v1;demosaic=bayer-edge-aware";
        break;
    case RawDemosaicAlgorithm::bayer_bilinear_v1:
        receipt.development_settings_signature = "shadow-raw-v1;demosaic=bayer-bilinear";
        break;
    }
    receipt.development_settings_signature += ";backend="
        + std::string(raw_development_backend_identity(backend));
    receipt.development_settings_signature += ";"
        + std::string(raw_highlight_treatment_identity(plan.highlight_recovery));
    receipt.development_settings_signature += ";" + raw_denoise.cache_identity;
    if (camera_profile != nullptr) {
        receipt.development_settings_signature += ";color=dcp;"
            + dcp_color_receipt_identity(camera_profile->receipt);
    } else {
        receipt.development_settings_signature += ";wb=as-shot;matrix=provider-generic";
    }
    receipt.requested_plan_identity = raw_development_plan_identity(plan);
    receipt.effective_plan_identity = receipt.requested_plan_identity;
    receipt.requested_plan = plan;
    receipt.effective_plan = plan;
    receipt.plan_negotiation_status = RawDevelopmentPlanNegotiationStatus::accepted;
    receipt.processed_linear_reference_contract_version =
        processed_linear_reference_rgb_contract_version;
    receipt.declared_image_dimensions = descriptor.active_dimensions;
    receipt.rendered_dimensions = rendered_dimensions;
    receipt.orientation = descriptor.orientation;
    receipt.half_size = rendered_dimensions != oriented_dimensions(
        descriptor.active_dimensions,
        descriptor.orientation
    );
    receipt.use_camera_white_balance = true;
    receipt.use_camera_matrix = true;
    receipt.use_auto_brightness = false;
    receipt.use_exposure_correction = false;
    receipt.brightness = 1.0F;
    receipt.maximum_adjustment_threshold = 0.0F;
    receipt.output_bits_per_channel = 32U;
    receipt.demosaic_quality = demosaic.algorithm == RawDemosaicAlgorithm::bayer_area_preview_v1
        ? 1 : demosaic.algorithm == RawDemosaicAlgorithm::bayer_edge_aware_v1 ? 4 : 3;
    receipt.output_color = 1;
    receipt.gamma_inverse_power = 1.0;
    receipt.gamma_linear_toe_slope = 1.0;
    receipt.declared_dng_opcode_lists = descriptor.declared_pending_corrections;
    for (std::size_t index = 0U; index < receipt.dng_opcode_execution.size(); ++index) {
        receipt.dng_opcode_execution[index] =
            descriptor.declared_pending_corrections.dng_opcode_list_bytes[index] == 0U
            ? DngOpcodeExecutionStatus::not_declared
            : DngOpcodeExecutionStatus::unsupported;
    }
    return receipt;
}

struct DevelopedRawFrame final {
    DevelopedSourcePixels source;
    RawDevelopmentReceipt raw_development_receipt;
    SensorClippingMask sensor_clipping_mask;
    RawDevelopmentBackend backend = RawDevelopmentBackend::cpu;
    RawHighlightRecoveryIntent highlight_recovery =
        RawHighlightRecoveryIntent::provider_default;
    std::string raw_denoise_cache_identity;
    double source_scene_luminance_percentile = 0.0;
};

[[nodiscard]] DevelopedRawFrame develop_raw_frame(
    RawFrame frame,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const DcpColorTransform* camera_profile,
    const double iso_sensitivity
) {
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "Shadow's RAW developer currently requires a valid Bayer two-by-two frame"
        );
    }
    if (!supported_orientation(frame.descriptor.orientation)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "Shadow's RAW developer does not support this source orientation"
        );
    }
    if (frame.descriptor.declared_pending_corrections.has_pending()) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RAW developer cannot yet execute this source's declared DNG opcodes"
        );
    }
    if (
        camera_profile == nullptr
        && !frame.descriptor.has_camera_to_linear_srgb_d65
        && !frame.descriptor.has_camera_to_xyz_d50
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RAW developer requires an explicit camera colour transform"
        );
    }

    const RawFrameLinearTransform transform = camera_profile == nullptr
        ? generic_raw_frame_transform(frame.descriptor)
        : RawFrameLinearTransform{camera_profile->camera_to_linear_srgb_d65};
    // Measure the source once before preview downsampling, CFA denoise, and the detail branch.
    // This is deliberately a calibration statistic, not a user auto-exposure operation.
    const double source_scene_luminance = sampled_scene_linear_luminance_percentile(
        frame,
        transform,
        camera_profile
    );
    // This is deliberately sampled before RAW-domain denoise. Zebra diagnostics describe
    // irreversible sensor clipping in the source CFA, not the values left after an optional
    // reconstruction aid. Keep this dimension calculation aligned with the fused developer's
    // preview and orientation policy without making a second source-frame copy.
    const Dimensions reconstruction_dimensions = preview_max_edge.has_value()
        ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
        : frame.descriptor.active_dimensions;
    const Dimensions diagnostic_dimensions =
        frame.descriptor.orientation == 5 || frame.descriptor.orientation == 6
        ? Dimensions{reconstruction_dimensions.height, reconstruction_dimensions.width}
        : reconstruction_dimensions;
    SensorClippingMask sensor_clipping_mask = project_sensor_clipping_mask(
        frame,
        diagnostic_dimensions
    );
    RawBayerDenoiseResult denoised = denoise_bayer_raw_frame(
        std::move(frame),
        RawBayerDenoiseRequest{
            .intent = plan.noise_reduction,
            .iso_sensitivity = iso_sensitivity,
            .preview = preview_max_edge.has_value(),
        }
    );
    FusedRawFrameDevelopment developed = develop_bayer_linear_srgb_f32_fused(
        denoised.frame,
        transform,
        preview_max_edge,
        plan.highlight_recovery,
        plan.quality
    );
    RawDevelopmentReceipt receipt = raw_frame_development_receipt(
        denoised.frame.descriptor,
        plan,
        developed.scene_linear.dimensions,
        developed.demosaic_receipt,
        developed.backend,
        camera_profile,
        denoised.receipt
    );
    DevelopedSourcePixels output = std::move(developed.scene_linear);
    DcpColorExecutionBackend dcp_execution_backend = DcpColorExecutionBackend::cpu;
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        // DCP's HueSatMap/LookTable/ProfileToneCurve define input rendering.
        // They intentionally run before the Recipe graph and are recorded in
        // the DCP receipt, rather than leaking camera-specific style into a
        // node the user might accidentally share across photos.
        dcp_execution_backend = apply_dcp_color_rendering_stages(
            std::get<SceneLinearRgbFrame>(output),
            *camera_profile
        );
    }
    if (camera_profile != nullptr) {
        // The CPU reference and Metal fp32 executor are both valid DCP renderers, but their
        // numerical paths are not assumed bit-identical.  Keep the effective executor in the
        // development signature so preview/detail/export caches cannot cross that boundary.
        receipt.development_settings_signature += ";"
            + std::string(dcp_color_execution_backend_identity(dcp_execution_backend));
    }
    return DevelopedRawFrame{
        .source = std::move(output),
        .raw_development_receipt = std::move(receipt),
        .sensor_clipping_mask = std::move(sensor_clipping_mask),
        .backend = developed.backend,
        .highlight_recovery = developed.highlight_recovery,
        .raw_denoise_cache_identity = denoised.receipt.cache_identity,
        .source_scene_luminance_percentile = source_scene_luminance,
    };
}

[[nodiscard]] DevelopedSourceReference provider_processed_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePath path,
    std::string fallback_reason
) {
    PixelBuffer pixels = preview_max_edge.has_value()
        ? session.render_reference_rgb_for_preview(*preview_max_edge, plan)
        : session.render_reference_rgb(plan);
    RawPipelineReceipt pipeline;
    pipeline.path = path;
    pipeline.pipeline_identity = path == RawPipelinePath::decoded_raster
        ? "shadow-decoded-raster-v1" : "shadow-provider-processed-compatibility-v1";
    pipeline.source_provider_id = pixels.raw_development_receipt.provider_id;
    pipeline.source_provider_version = pixels.raw_development_receipt.provider_version;
    pipeline.fallback_reason = std::move(fallback_reason);
    pipeline.requested_plan = plan;
    pipeline.effective_plan = pixels.raw_development_receipt.recorded()
        ? pixels.raw_development_receipt.effective_plan : plan;
    if (!pipeline.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "provider processed source produced an invalid RAW pipeline receipt"
        );
    }
    RawDevelopmentReceipt raw_development_receipt = pixels.raw_development_receipt;
    return DevelopedSourceReference{
        .source = std::move(pixels),
        .raw_development_receipt = std::move(raw_development_receipt),
        .pipeline_receipt = std::move(pipeline),
    };
}

[[nodiscard]] bool can_fallback_from(const DecodeError& error) noexcept {
    return error.code() == DecodeErrorCode::unsupported
        || error.code() == DecodeErrorCode::unsupported_layout;
}

} // namespace

RawDevelopmentCapabilities shadow_raw_frame_development_capabilities() noexcept {
    RawDevelopmentCapabilities capabilities;
    capabilities.available = true;
    capabilities.raw_frame = true;
    capabilities.supported_intents =
        raw_development_intent_mask(RawDevelopmentIntent::preview)
        | raw_development_intent_mask(RawDevelopmentIntent::detail)
        | raw_development_intent_mask(RawDevelopmentIntent::export_image);
    capabilities.supported_qualities =
        raw_development_quality_mask(RawDevelopmentQuality::balanced)
        | raw_development_quality_mask(RawDevelopmentQuality::high);
    capabilities.supported_dng_opcode_policies =
        dng_opcode_policy_mask(DngOpcodePolicy::provider_default);
    capabilities.supported_noise_reduction_intents =
        raw_noise_reduction_intent_mask(RawNoiseReductionIntent::provider_default)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::disabled)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::conservative)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::noise_robust);
    capabilities.supported_highlight_recovery_intents =
        raw_highlight_recovery_intent_mask(RawHighlightRecoveryIntent::provider_default)
        | raw_highlight_recovery_intent_mask(RawHighlightRecoveryIntent::disabled);
    return capabilities;
}

RawDevelopmentPlanNegotiation negotiate_shadow_raw_frame_development_plan(
    const RawDevelopmentPlan& requested
) noexcept {
    return shadow::image::negotiate_raw_development_plan(
        requested,
        shadow_raw_frame_development_capabilities()
    );
}

bool RawPipelineReceipt::valid() const noexcept {
    if (
        schema_version != raw_pipeline_receipt_schema_version || pipeline_identity.empty()
        || requested_plan.schema_version != raw_development_plan_schema_version
        || effective_plan.schema_version != raw_development_plan_schema_version
    ) {
        return false;
    }
    if (path == RawPipelinePath::shadow_raw_frame) {
        const bool raw_frame_is_valid =
            raw_frame_schema_version == shadow::image::raw_frame_schema_version
            && raw_developer_version == shadow_raw_frame_developer_version
            && fallback_reason.empty() && source_scene_luminance_percentile.has_value()
            && std::isfinite(*source_scene_luminance_percentile)
            && *source_scene_luminance_percentile >= 0.0;
        if (!raw_frame_is_valid) {
            return false;
        }
        switch (camera_profile_status) {
        case RawCameraProfileStatus::not_considered:
            return false;
        case RawCameraProfileStatus::no_match:
            return !camera_profile_catalog_identity.empty()
                && camera_profile_identity.empty() && camera_profile_name.empty()
                && camera_profile_diagnostic.empty()
                && camera_profile_developer_version == dcp_color_developer_version;
        case RawCameraProfileStatus::applied:
            return !camera_profile_catalog_identity.empty()
                && !camera_profile_identity.empty() && !camera_profile_name.empty()
                && camera_profile_diagnostic.empty()
                && camera_profile_developer_version == dcp_color_developer_version;
        case RawCameraProfileStatus::matched_not_applied:
            return !camera_profile_catalog_identity.empty()
                && !camera_profile_identity.empty() && !camera_profile_name.empty()
                && !camera_profile_diagnostic.empty()
                && camera_profile_developer_version == dcp_color_developer_version;
        }
        return false;
    }
    return raw_frame_schema_version == 0U && raw_developer_version == 0U
        && !source_scene_luminance_percentile.has_value()
        && camera_profile_status == RawCameraProfileStatus::not_considered
        && camera_profile_catalog_identity.empty() && camera_profile_identity.empty()
        && camera_profile_name.empty() && camera_profile_diagnostic.empty()
        && camera_profile_developer_version == 0U;
}

RawPipelinePolicy raw_pipeline_policy_from_environment() {
    const auto* configured = std::getenv(raw_pipeline_environment.data());
    if (configured == nullptr || *configured == '\0' || std::string_view(configured) == "auto") {
        return default_raw_pipeline_policy();
    }
    if (std::string_view(configured) == "raw-frame") {
        return RawPipelinePolicy{
            .schema_version = raw_pipeline_policy_schema_version,
            .mode = RawPipelineMode::require_shadow_raw_frame,
        };
    }
    if (std::string_view(configured) == "processed") {
        return RawPipelinePolicy{
            .schema_version = raw_pipeline_policy_schema_version,
            .mode = RawPipelineMode::require_provider_processed,
        };
    }
    throw DecodeError(
        DecodeErrorCode::invalid_request,
        0,
        "SHADOW_RAW_PIPELINE must be auto, raw-frame, or processed"
    );
}

std::string raw_pipeline_policy_identity(const RawPipelinePolicy& policy) {
    if (policy.schema_version != raw_pipeline_policy_schema_version) {
        throw std::invalid_argument("RAW pipeline policy schema is unsupported");
    }
    return "raw-pipeline-policy-v1;mode=" + std::string(mode_name(policy.mode));
}

std::string raw_pipeline_receipt_identity(const RawPipelineReceipt& receipt) {
    if (!receipt.valid()) {
        throw std::invalid_argument("RAW pipeline receipt is invalid");
    }
    std::ostringstream identity;
    identity << "raw-pipeline-receipt-v1;path=" << path_name(receipt.path)
             << ";pipeline=" << receipt.pipeline_identity
             << ";provider=" << receipt.source_provider_id
             << ";provider-version=" << receipt.source_provider_version
             << ";frame=" << receipt.raw_frame_schema_version
             << ";developer=" << receipt.raw_developer_version
             << ";effective=" << raw_development_plan_identity(receipt.effective_plan);
    if (receipt.source_scene_luminance_percentile.has_value()) {
        identity << ";source-luminance-p99=" << std::fixed << std::setprecision(8)
                 << *receipt.source_scene_luminance_percentile;
    }
    if (receipt.camera_profile_status != RawCameraProfileStatus::not_considered) {
        identity << ";camera-profile-status="
                 << camera_profile_status_name(receipt.camera_profile_status)
                 << ";camera-profile-catalog=" << receipt.camera_profile_catalog_identity
                 << ";camera-profile-developer="
                 << receipt.camera_profile_developer_version;
    }
    if (!receipt.camera_profile_identity.empty()) {
        identity << ";camera-profile=" << receipt.camera_profile_identity;
    }
    if (!receipt.fallback_reason.empty()) {
        identity << ";fallback=" << receipt.fallback_reason;
    }
    return identity.str();
}

DevelopedSourceReference develop_source_reference(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePolicy& policy
) {
    return develop_source_reference(
        session,
        plan,
        preview_max_edge,
        policy,
        default_camera_profile_catalog()
    );
}

DevelopedSourceReference develop_source_reference(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
) {
    if (
        policy.schema_version != raw_pipeline_policy_schema_version
        || plan.schema_version != raw_development_plan_schema_version
        || (preview_max_edge.has_value() && *preview_max_edge == 0U)
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "source development received an invalid plan, policy, or preview edge"
        );
    }
    const bool is_raw = session.raw_development_capabilities().available;
    if (!is_raw) {
        if (policy.mode == RawPipelineMode::require_shadow_raw_frame) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "a decoded raster cannot enter the Bayer RawFrame developer"
            );
        }
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::decoded_raster,
            {}
        );
    }
    if (policy.mode == RawPipelineMode::require_provider_processed) {
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::provider_processed_compatibility,
            "provider processed path was explicitly selected"
        );
    }
    if (!session.capabilities().raw_frame) {
        if (policy.mode == RawPipelineMode::require_shadow_raw_frame) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "the selected RAW provider cannot expose an owned RawFrame"
            );
        }
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::provider_processed_compatibility,
            "provider does not expose an owned RawFrame"
        );
    }

    try {
        const auto negotiation = negotiate_shadow_raw_frame_development_plan(plan);
        if (!negotiation.accepted()) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "Shadow's RawFrame developer cannot honor the requested development plan"
            );
        }
        RawFrame frame = session.decode_raw_frame();
        const auto source_provider_id = frame.descriptor.provider_id;
        const auto source_provider_version = frame.descriptor.provider_version;
        const auto camera_profile = match_camera_profile(camera_profiles, session.metadata());
        std::optional<DcpColorTransform> dcp_transform;
        RawCameraProfileStatus camera_profile_status = RawCameraProfileStatus::no_match;
        std::string camera_profile_identity;
        std::string camera_profile_name;
        std::string camera_profile_diagnostic;
        if (camera_profile != nullptr) {
            camera_profile_identity = camera_profile->content_identity;
            camera_profile_name = camera_profile->profile.profile_name.empty()
                ? camera_profile->profile.unique_camera_model
                : camera_profile->profile.profile_name;
            try {
                dcp_transform = compile_dcp_color_transform(*camera_profile, frame.descriptor);
                camera_profile_status = RawCameraProfileStatus::applied;
            } catch (const DcpColorDevelopmentError& profile_error) {
                // Optional local profiles are an enhancement, not a prerequisite for decoding.
                // Keep the generic provider matrix and record the full diagnostic rather than
                // silently claiming camera rendering that the local DCP could not compile.
                camera_profile_status = RawCameraProfileStatus::matched_not_applied;
                camera_profile_diagnostic = profile_error.what();
            }
        }
        DevelopedRawFrame developed = develop_raw_frame(
            std::move(frame),
            negotiation.effective,
            preview_max_edge,
            dcp_transform.has_value() ? &*dcp_transform : nullptr,
            session.metadata().iso_speed
        );
        RawDevelopmentReceipt raw_development_receipt = std::move(
            developed.raw_development_receipt
        );
        raw_development_receipt.requested_plan = plan;
        raw_development_receipt.requested_plan_identity =
            raw_development_plan_identity(plan);
        raw_development_receipt.effective_plan = negotiation.effective;
        raw_development_receipt.effective_plan_identity =
            raw_development_plan_identity(negotiation.effective);
        raw_development_receipt.plan_negotiation_status = negotiation.status;
        RawPipelineReceipt pipeline;
        pipeline.path = RawPipelinePath::shadow_raw_frame;
        pipeline.pipeline_identity = std::string(raw_frame_pipeline_identity);
        pipeline.pipeline_identity += ";backend="
            + std::string(raw_development_backend_identity(developed.backend));
        pipeline.pipeline_identity += ";"
            + std::string(raw_highlight_treatment_identity(developed.highlight_recovery));
        pipeline.pipeline_identity += ";" + developed.raw_denoise_cache_identity;
        pipeline.source_provider_id = source_provider_id;
        pipeline.source_provider_version = source_provider_version;
        pipeline.raw_frame_schema_version = raw_frame_schema_version;
        pipeline.raw_developer_version = shadow_raw_frame_developer_version;
        pipeline.source_scene_luminance_percentile =
            developed.source_scene_luminance_percentile;
        pipeline.requested_plan = plan;
        pipeline.effective_plan = negotiation.effective;
        pipeline.camera_profile_status = camera_profile_status;
        pipeline.camera_profile_catalog_identity = camera_profiles.identity;
        pipeline.camera_profile_identity = std::move(camera_profile_identity);
        pipeline.camera_profile_name = std::move(camera_profile_name);
        pipeline.camera_profile_diagnostic = std::move(camera_profile_diagnostic);
        pipeline.camera_profile_developer_version = dcp_color_developer_version;
        pipeline.pipeline_identity += ";camera-profile-status="
            + std::string(camera_profile_status_name(pipeline.camera_profile_status))
            + ";camera-profile-catalog=" + pipeline.camera_profile_catalog_identity;
        if (!pipeline.camera_profile_identity.empty()) {
            pipeline.pipeline_identity += ";camera-profile="
                + pipeline.camera_profile_identity;
        }
        if (!pipeline.valid()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Shadow RawFrame development produced an invalid pipeline receipt"
            );
        }
        return DevelopedSourceReference{
            .source = std::move(developed.source),
            .raw_development_receipt = std::move(raw_development_receipt),
            .pipeline_receipt = std::move(pipeline),
            .sensor_clipping_mask = std::move(developed.sensor_clipping_mask),
        };
    } catch (const DecodeError& error) {
        if (
            policy.mode == RawPipelineMode::require_shadow_raw_frame
            || !can_fallback_from(error)
        ) {
            throw;
        }
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::provider_processed_compatibility,
            error.what()
        );
    }
}

} // namespace shadow::image

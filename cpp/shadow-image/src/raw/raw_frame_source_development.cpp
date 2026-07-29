#include "raw_frame_source_development.hpp"

#include "bayer_sampling.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_denoise.hpp>

#include "metal_raw_development.hpp"
#include "raw_denoise_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image::raw_pipeline_detail {

namespace {

inline constexpr std::uint32_t source_luminance_sample_edge = 256U;
inline constexpr double source_luminance_percentile = 0.990;

[[nodiscard]] bool supported_orientation(const std::int32_t orientation) noexcept {
    return orientation == 0 || orientation == 3 || orientation == 5 || orientation == 6;
}

[[nodiscard]] Dimensions
oriented_dimensions(const Dimensions dimensions, const std::int32_t orientation) noexcept {
    if (orientation == 5 || orientation == 6) {
        return Dimensions{dimensions.height, dimensions.width};
    }
    return dimensions;
}

[[nodiscard]] std::array<double, 3U>
canonical_camera_neutral(const RawFrameDescriptor& descriptor) {
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

[[nodiscard]] std::array<double, 3U>
white_balance_multipliers(const RawFrameDescriptor& descriptor) {
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

[[nodiscard]] Matrix3 multiply_matrix(const Matrix3& left, const Matrix3& right) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] += left[row * 3U + inner] * right[inner * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] RawFrameLinearTransform
generic_raw_frame_transform(const RawFrameDescriptor& descriptor) {
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
            0.9555766,
            -0.0230393,
            0.0631636,
            -0.0282895,
            1.0099416,
            0.0210077,
            0.0122982,
            -0.0204830,
            1.3299098,
        };
        constexpr Matrix3 xyz_d65_to_srgb{
            3.2404542,
            -1.5371385,
            -0.4985314,
            -0.9692660,
            1.8760108,
            0.0415560,
            0.0556434,
            -0.2040259,
            1.0572252,
        };
        camera_to_srgb =
            multiply_matrix(xyz_d65_to_srgb, multiply_matrix(d50_to_d65, camera_to_xyz_d50));
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
    samples.samples.resize(static_cast<std::size_t>(sample_columns) * sample_rows * 3U);
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
        const std::uint32_t raw_y =
            frame.descriptor.active_margins.top + source_coordinate(y, sample_rows, active.height);
        for (std::uint32_t x = 0U; x < sample_columns; ++x) {
            const std::uint32_t raw_x = frame.descriptor.active_margins.left
                                        + source_coordinate(x, sample_columns, active.width);
            const auto camera = detail::bilinear_camera_rgb_at(frame, raw_x, raw_y);
            const std::size_t index = (static_cast<std::size_t>(y) * sample_columns + x) * 3U;
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
        static_cast<std::size_t>(
            std::floor(static_cast<double>(luminances.size() - 1U) * source_luminance_percentile)
        )
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
    receipt.provider_id =
        descriptor.provider_id.empty() ? "provider-neutral-raw-frame" : descriptor.provider_id;
    receipt.provider_version =
        descriptor.provider_version.empty() ? "unrecorded" : descriptor.provider_version;
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
    receipt.development_settings_signature +=
        ";backend=" + std::string(raw_development_backend_identity(backend));
    receipt.development_settings_signature +=
        ";" + std::string(raw_highlight_treatment_identity(plan.highlight_recovery));
    receipt.development_settings_signature += ";" + raw_denoise.cache_identity;
    if (camera_profile != nullptr) {
        receipt.development_settings_signature +=
            ";color=dcp;" + dcp_color_receipt_identity(camera_profile->receipt);
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
    receipt.half_size =
        rendered_dimensions
        != oriented_dimensions(descriptor.active_dimensions, descriptor.orientation);
    receipt.use_camera_white_balance = true;
    receipt.use_camera_matrix = true;
    receipt.use_auto_brightness = false;
    receipt.use_exposure_correction = false;
    receipt.brightness = 1.0F;
    receipt.maximum_adjustment_threshold = 0.0F;
    receipt.output_bits_per_channel = 32U;
    receipt.demosaic_quality = demosaic.algorithm == RawDemosaicAlgorithm::bayer_area_preview_v1 ? 1
                               : demosaic.algorithm == RawDemosaicAlgorithm::bayer_edge_aware_v1
                                   ? 4
                                   : 3;
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

} // namespace

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
    if (camera_profile == nullptr && !frame.descriptor.has_camera_to_linear_srgb_d65
        && !frame.descriptor.has_camera_to_xyz_d50) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RAW developer requires an explicit camera colour transform"
        );
    }

    const RawFrameLinearTransform transform =
        camera_profile == nullptr
            ? generic_raw_frame_transform(frame.descriptor)
            : RawFrameLinearTransform{camera_profile->camera_to_linear_srgb_d65};
    // Measure the source once before preview downsampling, CFA denoise, and the detail branch.
    // This is deliberately a calibration statistic, not a user auto-exposure operation.
    const double source_scene_luminance =
        sampled_scene_linear_luminance_percentile(frame, transform, camera_profile);
    // This is deliberately sampled before RAW-domain denoise. Zebra diagnostics describe
    // irreversible sensor clipping in the source CFA, not the values left after an optional
    // reconstruction aid. Keep this dimension calculation aligned with the fused developer's
    // preview and orientation policy without making a second source-frame copy.
    const Dimensions reconstruction_dimensions =
        preview_max_edge.has_value()
            ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
            : frame.descriptor.active_dimensions;
    const Dimensions diagnostic_dimensions =
        frame.descriptor.orientation == 5 || frame.descriptor.orientation == 6
            ? Dimensions{reconstruction_dimensions.height, reconstruction_dimensions.width}
            : reconstruction_dimensions;
    std::optional<SensorClippingMask> sensor_clipping_mask;
    const RawBayerDenoiseRequest raw_denoise_request{
        .intent = plan.noise_reduction,
        .iso_sensitivity = iso_sensitivity,
        .preview = preview_max_edge.has_value(),
    };
    const auto prepared_raw_denoise = detail::prepare_raw_bayer_denoise(frame, raw_denoise_request);
    std::optional<FusedRawFrameDevelopment> prepared_development;
    std::optional<RawBayerDenoiseResult> materialized_raw_denoise;
    RawBayerDenoiseReceipt raw_denoise_receipt;
    bool fused_dcp_applied = false;
    const RawDevelopmentBackendMode requested_backend =
        raw_development_backend_mode_from_environment();
    const bool dcp_requested =
        camera_profile != nullptr && camera_profile->has_post_matrix_stages();
    if (requested_backend != RawDevelopmentBackendMode::cpu) {
        auto fused_attempt = detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            preview_max_edge,
            plan.highlight_recovery,
            plan.quality,
            detail::MetalRawDevelopmentContinuations{
                .dcp_color_transform = dcp_requested ? camera_profile : nullptr,
                .raw_denoise = prepared_raw_denoise.applied() ? &prepared_raw_denoise : nullptr,
                .project_sensor_clipping = true,
            }
        );
        if (fused_attempt.development.has_value() && fused_attempt.sensor_clipping_mask.has_value()
            && fused_attempt.raw_denoise_applied == prepared_raw_denoise.applied()
            && fused_attempt.dcp_applied == dcp_requested) {
            prepared_development = std::move(fused_attempt.development);
            sensor_clipping_mask = std::move(fused_attempt.sensor_clipping_mask);
            fused_dcp_applied = fused_attempt.dcp_applied;
            raw_denoise_receipt = detail::finalize_raw_bayer_denoise_receipt(
                frame,
                prepared_raw_denoise,
                prepared_raw_denoise.applied() ? RawBayerDenoiseBackend::metal
                                               : RawBayerDenoiseBackend::cpu
            );
        }
    }
    if (!prepared_development.has_value()) {
        sensor_clipping_mask = project_sensor_clipping_mask(frame, diagnostic_dimensions);
        materialized_raw_denoise =
            detail::execute_prepared_raw_bayer_denoise(std::move(frame), prepared_raw_denoise);
        raw_denoise_receipt = materialized_raw_denoise->receipt;
        prepared_development = develop_bayer_linear_srgb_f32_fused(
            materialized_raw_denoise->frame,
            transform,
            preview_max_edge,
            plan.highlight_recovery,
            plan.quality
        );
    }
    FusedRawFrameDevelopment developed = std::move(*prepared_development);
    const RawFrameDescriptor& developed_descriptor =
        materialized_raw_denoise.has_value() ? materialized_raw_denoise->frame.descriptor
                                             : frame.descriptor;
    RawDevelopmentReceipt receipt = raw_frame_development_receipt(
        developed_descriptor,
        plan,
        developed.scene_linear.dimensions,
        developed.demosaic_receipt,
        developed.backend,
        camera_profile,
        raw_denoise_receipt
    );
    DevelopedSourcePixels output = std::move(developed.scene_linear);
    DcpColorExecutionBackend dcp_execution_backend =
        fused_dcp_applied ? DcpColorExecutionBackend::metal : DcpColorExecutionBackend::cpu;
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        // DCP's HueSatMap/LookTable/ProfileToneCurve define input rendering.
        // They intentionally run before the Recipe graph and are recorded in
        // the DCP receipt, rather than leaking camera-specific style into a
        // node the user might accidentally share across photos.
        if (!fused_dcp_applied) {
            dcp_execution_backend = apply_dcp_color_rendering_stages(
                std::get<SceneLinearRgbFrame>(output),
                *camera_profile
            );
        }
    }
    if (camera_profile != nullptr) {
        // The CPU reference and Metal fp32 executor are both valid DCP renderers, but their
        // numerical paths are not assumed bit-identical.  Keep the effective executor in the
        // development signature so preview/detail/export caches cannot cross that boundary.
        receipt.development_settings_signature +=
            ";" + std::string(dcp_color_execution_backend_identity(dcp_execution_backend));
    }
    return DevelopedRawFrame{
        .source = std::move(output),
        .raw_development_receipt = std::move(receipt),
        .sensor_clipping_mask = std::move(*sensor_clipping_mask),
        .backend = developed.backend,
        .highlight_recovery = developed.highlight_recovery,
        .raw_denoise_cache_identity = raw_denoise_receipt.cache_identity,
        .source_scene_luminance_percentile = source_scene_luminance,
    };
}

} // namespace shadow::image::raw_pipeline_detail

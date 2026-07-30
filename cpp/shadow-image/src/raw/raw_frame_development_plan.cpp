#include "raw_frame_development_plan.hpp"

#include "bayer_sampling.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/proxy_rendering.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] std::array<double, 3U> canonical_camera_neutral(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) {
    if (!valid_raw_white_balance(white_balance)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW frame development requires a canonical white balance"
        );
    }
    if (white_balance.mode == RawWhiteBalanceMode::camera_neutral) {
        return {
            static_cast<double>(white_balance.camera_neutral_red_millionths)
                / static_cast<double>(raw_camera_neutral_millionths),
            1.0,
            static_cast<double>(white_balance.camera_neutral_blue_millionths)
                / static_cast<double>(raw_camera_neutral_millionths),
        };
    }

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
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) {
    const auto neutral = canonical_camera_neutral(descriptor, white_balance);
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

[[nodiscard]] RawFrameLinearTransform generic_raw_frame_transform(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
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

    const auto multipliers = white_balance_multipliers(descriptor, white_balance);
    // Fold WB into the input columns so the hot loop performs one matrix multiply.
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            camera_to_srgb[output * 3U + input] *= multipliers[input];
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

} // namespace

PreparedRawFrameDevelopment::PreparedRawFrameDevelopment(
    RawFrameDescriptor descriptor,
    RawDevelopmentPlan development_plan,
    const std::optional<std::uint32_t> preview_max_edge,
    RawFrameLinearTransform linear_transform,
    std::optional<DcpColorTransform> camera_profile,
    detail::PreparedNeuralRawDenoise neural_raw_denoise,
    detail::PreparedRawBayerDenoise raw_denoise,
    const RawDevelopmentBackendMode requested_backend,
    const Dimensions reconstruction_dimensions,
    const Dimensions diagnostic_dimensions,
    const double source_scene_luminance_percentile
) :
    descriptor_(std::move(descriptor)), development_plan_(development_plan),
    preview_max_edge_(preview_max_edge), linear_transform_(linear_transform),
    camera_profile_(std::move(camera_profile)), neural_raw_denoise_(std::move(neural_raw_denoise)),
    raw_denoise_(std::move(raw_denoise)), requested_backend_(requested_backend),
    reconstruction_dimensions_(reconstruction_dimensions),
    diagnostic_dimensions_(diagnostic_dimensions),
    source_scene_luminance_percentile_(source_scene_luminance_percentile) {}

const RawFrameDescriptor& PreparedRawFrameDevelopment::descriptor() const noexcept {
    return descriptor_;
}

const RawDevelopmentPlan& PreparedRawFrameDevelopment::development_plan() const noexcept {
    return development_plan_;
}

std::optional<std::uint32_t> PreparedRawFrameDevelopment::preview_max_edge() const noexcept {
    return preview_max_edge_;
}

const RawFrameLinearTransform& PreparedRawFrameDevelopment::linear_transform() const noexcept {
    return linear_transform_;
}

const DcpColorTransform* PreparedRawFrameDevelopment::camera_profile() const noexcept {
    return camera_profile_.has_value() ? &*camera_profile_ : nullptr;
}

const detail::PreparedNeuralRawDenoise&
PreparedRawFrameDevelopment::neural_raw_denoise() const noexcept {
    return neural_raw_denoise_;
}

const detail::PreparedRawBayerDenoise& PreparedRawFrameDevelopment::raw_denoise() const noexcept {
    return raw_denoise_;
}

RawDevelopmentBackendMode PreparedRawFrameDevelopment::requested_backend() const noexcept {
    return requested_backend_;
}

Dimensions PreparedRawFrameDevelopment::reconstruction_dimensions() const noexcept {
    return reconstruction_dimensions_;
}

Dimensions PreparedRawFrameDevelopment::diagnostic_dimensions() const noexcept {
    return diagnostic_dimensions_;
}

double PreparedRawFrameDevelopment::source_scene_luminance_percentile() const noexcept {
    return source_scene_luminance_percentile_;
}

PreparedRawFrameDevelopment prepare_raw_frame_development(
    const RawFrame& frame,
    RawDevelopmentPlan development_plan,
    const std::optional<std::uint32_t> preview_max_edge,
    std::optional<DcpColorTransform> camera_profile,
    const double iso_sensitivity
) {
    if (development_plan.schema_version != raw_development_plan_schema_version
        || (preview_max_edge.has_value() && *preview_max_edge == 0U)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW frame development preparation received an invalid plan or preview edge"
        );
    }
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
    if (!camera_profile.has_value() && !frame.descriptor.has_camera_to_linear_srgb_d65
        && !frame.descriptor.has_camera_to_xyz_d50) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RAW developer requires an explicit camera colour transform"
        );
    }

    const RawFrameLinearTransform transform =
        camera_profile.has_value()
            ? RawFrameLinearTransform{camera_profile->camera_to_linear_srgb_d65}
            : generic_raw_frame_transform(frame.descriptor, development_plan.white_balance);
    const DcpColorTransform* camera_profile_ptr =
        camera_profile.has_value() ? &*camera_profile : nullptr;
    // Measure the source once before preview downsampling, CFA denoise, and the detail branch.
    // This is deliberately a calibration statistic, not a user auto-exposure operation.
    const double scene_luminance =
        sampled_scene_linear_luminance_percentile(frame, transform, camera_profile_ptr);
    // This diagnostic also precedes RAW-domain denoise: it describes irreversible sensor clipping,
    // not values left after an optional reconstruction aid.
    const Dimensions reconstruction_dimensions =
        preview_max_edge.has_value()
            ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
            : frame.descriptor.active_dimensions;
    const Dimensions diagnostic_dimensions =
        oriented_dimensions(reconstruction_dimensions, frame.descriptor.orientation);
    const auto raw_denoise = detail::prepare_raw_bayer_denoise(
        frame,
        RawBayerDenoiseRequest{
            .intent = development_plan.noise_reduction,
            .iso_sensitivity = iso_sensitivity,
            .preview = preview_max_edge.has_value(),
        }
    );
    const auto neural_raw_denoise =
        detail::prepare_neural_raw_denoise_from_environment(frame, preview_max_edge.has_value());

    return PreparedRawFrameDevelopment(
        frame.descriptor,
        development_plan,
        preview_max_edge,
        transform,
        std::move(camera_profile),
        neural_raw_denoise,
        raw_denoise,
        raw_development_backend_mode_from_environment(),
        reconstruction_dimensions,
        diagnostic_dimensions,
        scene_luminance
    );
}

RawDevelopmentReceipt finalize_raw_frame_development_receipt(
    const PreparedRawFrameDevelopment& prepared,
    const Dimensions rendered_dimensions,
    const RawDemosaicReceipt& demosaic,
    const RawDevelopmentBackend backend,
    const detail::NeuralRawDenoiseReceipt& neural_raw_denoise,
    const RawBayerDenoiseReceipt& raw_denoise,
    const DcpColorExecutionBackend dcp_execution_backend
) {
    const RawFrameDescriptor& descriptor = prepared.descriptor();
    const RawDevelopmentPlan& development_plan = prepared.development_plan();
    const DcpColorTransform* camera_profile = prepared.camera_profile();
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
        ";" + std::string(raw_highlight_treatment_identity(development_plan.highlight_recovery));
    receipt.development_settings_signature +=
        ";" + detail::combined_raw_denoise_cache_identity(neural_raw_denoise, raw_denoise);
    if (camera_profile != nullptr) {
        receipt.development_settings_signature +=
            ";color=dcp;" + dcp_color_receipt_identity(camera_profile->receipt);
        receipt.development_settings_signature +=
            ";" + std::string(dcp_color_execution_backend_identity(dcp_execution_backend));
    } else {
        receipt.development_settings_signature +=
            development_plan.white_balance.mode == RawWhiteBalanceMode::as_shot
                ? ";wb=as-shot;matrix=provider-generic"
                : ";wb=camera-neutral;matrix=provider-generic";
    }
    receipt.requested_plan_identity = raw_development_plan_identity(development_plan);
    receipt.effective_plan_identity = receipt.requested_plan_identity;
    receipt.requested_plan = development_plan;
    receipt.effective_plan = development_plan;
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

} // namespace shadow::image::raw_pipeline_detail

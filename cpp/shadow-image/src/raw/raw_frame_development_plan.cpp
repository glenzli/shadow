#include "raw_frame_development_plan.hpp"

#include "bayer_sampling.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_white_balance.hpp>

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
    const auto neutral = raw_frame_camera_neutral(descriptor, white_balance);
    if (!neutral.has_value()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "RAW frame cannot resolve the requested photographic white balance"
        );
    }
    return *neutral;
}

[[nodiscard]] std::array<double, 4U>
inverse_cfa_neutral(std::array<double, 4U> neutral) {
    for (double& value : neutral) {
        if (!std::isfinite(value) || value <= 0.0) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame camera neutral produces an invalid CFA white balance"
            );
        }
        value = 1.0 / value;
    }
    return neutral;
}

[[nodiscard]] std::array<double, 4U> cfa_white_balance_multipliers_from_camera_neutral(
    const RawFrameDescriptor& descriptor,
    const std::array<double, 3U>& camera_neutral
) {
    std::array<double, 4U> cfa_neutral{};
    for (std::size_t site = 0U; site < cfa_neutral.size(); ++site) {
        switch (descriptor.bayer_2x2[site]) {
        case RawCfaColor::red:
            cfa_neutral[site] = camera_neutral[0U];
            break;
        case RawCfaColor::green:
            cfa_neutral[site] = camera_neutral[1U];
            break;
        case RawCfaColor::blue:
            cfa_neutral[site] = camera_neutral[2U];
            break;
        case RawCfaColor::unknown:
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame white balance encountered an unknown CFA colour"
            );
        }
    }
    return inverse_cfa_neutral(cfa_neutral);
}

[[nodiscard]] std::array<double, 4U> cfa_white_balance_multipliers(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) {
    // Preserve the two independently recorded green sites for AsShot. A manual balance has one
    // green-normalized camera neutral and can be expanded across the CFA pattern.
    if (white_balance.mode == RawWhiteBalanceMode::as_shot) {
        return inverse_cfa_neutral(descriptor.as_shot_neutral);
    }
    return cfa_white_balance_multipliers_from_camera_neutral(
        descriptor,
        canonical_camera_neutral(descriptor, white_balance)
    );
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

// DCP's compiled matrix maps native, un-white-balanced camera RGB to linear sRGB. RawFrame
// reconstruction applies the selected gains at CFA sites before demosaic, so bind the inverse
// input basis into the matrix here: M(native) == M * diag(neutral)(balanced). This preserves the
// DCP's calibrated colour result for unsaturated samples while retaining CFA-domain white balance
// where sensor clipping is still observable.
[[nodiscard]] Matrix3 bind_camera_matrix_after_cfa_white_balance(
    const Matrix3& native_camera_to_srgb,
    const std::array<double, 3U>& camera_neutral
) {
    Matrix3 bound = native_camera_to_srgb;
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            bound[output * 3U + input] *= camera_neutral[input];
        }
    }
    return bound;
}

// AI foundations retain linear Camera RGB reconstructed before Shadow's CFA-site gains. Recreate
// the same camera-domain white balance in the matrix that consumes that raster. This is the inverse
// of the CFA-basis binding above: a DCP matrix already bound as M * diag(neutral) resolves back to
// its native M, while a generic/decoder matrix M becomes M * diag(1 / neutral).
[[nodiscard]] Matrix3 bind_camera_rgb_matrix_before_cfa_white_balance(
    const Matrix3& cfa_balanced_camera_to_srgb,
    const std::array<double, 3U>& camera_neutral
) {
    Matrix3 bound = cfa_balanced_camera_to_srgb;
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            bound[output * 3U + input] /= camera_neutral[input];
        }
    }
    return bound;
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

    const auto neutral = canonical_camera_neutral(descriptor, white_balance);
    return RawFrameLinearTransform{
        .camera_to_linear_srgb_d65 = camera_to_srgb,
        .camera_rgb_to_linear_srgb_d65 =
            bind_camera_rgb_matrix_before_cfa_white_balance(camera_to_srgb, neutral),
        .camera_neutral = neutral,
        .cfa_white_balance = cfa_white_balance_multipliers(descriptor, white_balance),
        .apply_cfa_white_balance = true,
    };
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
            const auto camera = detail::bilinear_camera_rgb_at(frame, raw_x, raw_y, &transform);
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
    detail::PreparedRawBayerDenoise raw_denoise,
    std::shared_ptr<const detail::CfaOpposedChrominanceModel> highlight_chrominance_model,
    const RawDevelopmentBackendMode requested_backend,
    const Dimensions reconstruction_dimensions,
    const Dimensions diagnostic_dimensions,
    const double source_scene_luminance_percentile
) :
    descriptor_(std::move(descriptor)), development_plan_(development_plan),
    preview_max_edge_(preview_max_edge), linear_transform_(linear_transform),
    camera_profile_(std::move(camera_profile)), raw_denoise_(std::move(raw_denoise)),
    highlight_chrominance_model_(std::move(highlight_chrominance_model)),
    requested_backend_(requested_backend), reconstruction_dimensions_(reconstruction_dimensions),
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

PreparedRawFrameDevelopment PreparedRawFrameDevelopment::rebind_color(
    const RawDevelopmentPlan development_plan,
    RawFrameLinearTransform linear_transform,
    std::optional<DcpColorTransform> camera_profile,
    const double source_scene_luminance_percentile
) const {
    RawDevelopmentPlan fixed_binding = development_plan_;
    fixed_binding.white_balance = development_plan.white_balance;
    if (development_plan != fixed_binding || !linear_transform.valid()
        || !std::isfinite(source_scene_luminance_percentile)
        || source_scene_luminance_percentile < 0.0) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW preview colour rebinding may change only a canonical white balance"
        );
    }
    if (highlight_chrominance_model_ != nullptr) {
        const auto policy = detail::editable_raw_cfa_sampling_policy(linear_transform);
        linear_transform.opposed_highlight_chrominance_offsets =
            detail::evaluate_opposed_highlight_chrominance_model(
                *highlight_chrominance_model_,
                &linear_transform,
                policy
            )
                .offsets;
    }
    return PreparedRawFrameDevelopment(
        descriptor_,
        development_plan,
        preview_max_edge_,
        std::move(linear_transform),
        std::move(camera_profile),
        raw_denoise_,
        highlight_chrominance_model_,
        requested_backend_,
        reconstruction_dimensions_,
        diagnostic_dimensions_,
        source_scene_luminance_percentile
    );
}

RawFrameLinearTransform prepare_raw_frame_linear_transform(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance,
    const DcpColorTransform* camera_profile
) {
    if (!valid_raw_white_balance(white_balance)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW frame colour binding requires a canonical white balance"
        );
    }
    if (camera_profile != nullptr) {
        if (!camera_profile->valid()) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "RAW frame colour binding received an invalid DCP transform"
            );
        }
        // The DCP compiler is the authority for the native camera neutral of a manual
        // temperature/tint. Its profile-calibrated solve is not interchangeable with the generic
        // descriptor matrix used by the no-DCP route.
        const auto neutral = camera_profile->camera_neutral;
        // LibRaw's per-file camera matrix is the primary calibration when it is available. A
        // DCP supplies the authored neutral and optional input-rendering stages, but its
        // ForwardMatrix is not a substitute for a source-specific matrix supplied by the
        // decoder. This also keeps a profile from changing a proven camera rendering merely by
        // being installed locally.
        if (descriptor.has_camera_to_linear_srgb_d65) {
            return RawFrameLinearTransform{
                .camera_to_linear_srgb_d65 = descriptor.camera_to_linear_srgb_d65,
                .camera_rgb_to_linear_srgb_d65 = bind_camera_rgb_matrix_before_cfa_white_balance(
                    descriptor.camera_to_linear_srgb_d65,
                    neutral
                ),
                .camera_neutral = neutral,
                .cfa_white_balance =
                    white_balance.mode == RawWhiteBalanceMode::as_shot
                        ? cfa_white_balance_multipliers(descriptor, white_balance)
                        : cfa_white_balance_multipliers_from_camera_neutral(descriptor, neutral),
                .apply_cfa_white_balance = true,
            };
        }
        const Matrix3 cfa_balanced_camera_to_srgb = bind_camera_matrix_after_cfa_white_balance(
            camera_profile->camera_to_linear_srgb_d65,
            neutral
        );
        return RawFrameLinearTransform{
            .camera_to_linear_srgb_d65 = cfa_balanced_camera_to_srgb,
            .camera_rgb_to_linear_srgb_d65 = bind_camera_rgb_matrix_before_cfa_white_balance(
                cfa_balanced_camera_to_srgb,
                neutral
            ),
            .camera_neutral = neutral,
            .cfa_white_balance =
                white_balance.mode == RawWhiteBalanceMode::as_shot
                    ? cfa_white_balance_multipliers(descriptor, white_balance)
                    : cfa_white_balance_multipliers_from_camera_neutral(descriptor, neutral),
            .apply_cfa_white_balance = true,
        };
    }
    return generic_raw_frame_transform(descriptor, white_balance);
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

    RawFrameLinearTransform transform = prepare_raw_frame_linear_transform(
        frame.descriptor,
        development_plan.white_balance,
        camera_profile.has_value() ? &*camera_profile : nullptr
    );
    if (camera_profile.has_value() && frame.descriptor.has_camera_to_linear_srgb_d65) {
        // A decoder-provided camera->linear-sRGB matrix is already a complete camera rendering
        // basis. DCP HueSatMap/LookTable are authored in the output space of the DCP's own Forward
        // (or inverse Color) matrix, so applying them after that independent decoder matrix mixes
        // calibrated coordinate systems and can turn neutral highlights strongly chromatic. The DCP
        // remains the authority for the manual camera neutral; only its output-space look stages are
        // inapplicable on this route.
        camera_profile->clear_post_matrix_stages();
    }
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
    std::shared_ptr<const detail::CfaOpposedChrominanceModel> highlight_chrominance_model;
    if (development_plan.highlight_recovery == RawHighlightRecoveryIntent::provider_default
        || development_plan.highlight_recovery == RawHighlightRecoveryIntent::aggressive) {
        auto compiled_model = std::make_shared<detail::CfaOpposedChrominanceModel>(
            detail::build_opposed_highlight_chrominance_model(frame)
        );
        const auto policy = detail::editable_raw_cfa_sampling_policy(transform);
        transform.opposed_highlight_chrominance_offsets =
            detail::evaluate_opposed_highlight_chrominance_model(
                *compiled_model,
                &transform,
                policy
            )
                .offsets;
        highlight_chrominance_model = std::move(compiled_model);
    }
    return PreparedRawFrameDevelopment(
        frame.descriptor,
        development_plan,
        preview_max_edge,
        transform,
        std::move(camera_profile),
        raw_denoise,
        std::move(highlight_chrominance_model),
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
    receipt.development_settings_signature += ";" + raw_denoise.cache_identity;
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

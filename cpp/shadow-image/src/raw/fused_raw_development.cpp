#include <shadow/image/fused_raw_development.hpp>

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/proxy_rendering.hpp>

#include "../acceleration/image_acceleration_policy.hpp"
#include "../concurrency/row_scheduler.hpp"
#include "bayer_sampling.hpp"
#include "metal_raw_development.hpp"
#include "raw_frame_region_development.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace shadow::image {

namespace {

void validate_request(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawDevelopmentQuality quality,
    const RawHighlightRecoveryIntent highlight_recovery
) {
    detail::validate_bayer_frame(frame, "fused Bayer development");
    if (!transform.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "fused Bayer development requires a finite non-zero camera transform"
        );
    }
    const auto orientation = frame.descriptor.orientation;
    if (orientation != 0 && orientation != 3 && orientation != 5 && orientation != 6) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "fused Bayer development does not support this source orientation"
        );
    }
    if (preview_max_edge.has_value() && *preview_max_edge == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "fused Bayer preview max edge must be non-zero"
        );
    }
    if (quality != RawDevelopmentQuality::fast && quality != RawDevelopmentQuality::balanced
        && quality != RawDevelopmentQuality::high) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "fused Bayer development received an unknown RAW quality tier"
        );
    }
    if (highlight_recovery != RawHighlightRecoveryIntent::provider_default
        && highlight_recovery != RawHighlightRecoveryIntent::disabled
        && highlight_recovery != RawHighlightRecoveryIntent::aggressive) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "fused Bayer development does not implement the requested highlight recovery"
        );
    }
}

[[nodiscard]] Dimensions
oriented_dimensions(const Dimensions dimensions, const std::int32_t orientation) noexcept {
    return raw_pipeline_detail::oriented_raw_dimensions(dimensions, orientation);
}

// Maps one display-oriented output coordinate back to the un-oriented reconstruction raster.
// These are the same LibRaw orientation semantics used by the existing RawFrame developer.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> source_coordinate(
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const Dimensions source,
    const std::int32_t orientation
) noexcept {
    switch (orientation) {
    case 3:
        return {source.width - 1U - output_x, source.height - 1U - output_y};
    case 5:
        return {source.width - 1U - output_y, output_x};
    case 6:
        return {output_y, source.height - 1U - output_x};
    case 0:
    default:
        return {output_x, output_y};
    }
}

[[nodiscard]] SceneLinearRgbFrame allocate_output(const Dimensions dimensions) {
    const auto sample_count = static_cast<std::uint64_t>(dimensions.width) * dimensions.height * 3U;
    if (sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "fused Bayer development output exceeds the address space"
        );
    }
    SceneLinearRgbFrame output;
    output.dimensions = dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    output.samples.resize(static_cast<std::size_t>(sample_count));
    return output;
}

[[nodiscard]] RawDemosaicReceipt make_area_demosaic_receipt(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform
) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = RawDemosaicAlgorithm::bayer_area_preview_v1,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = transform.apply_cfa_white_balance,
        .dng_opcodes_applied = false,
    };
}

[[nodiscard]] bool valid_demosaic_receipt(const RawDemosaicReceipt& receipt) noexcept {
    return receipt.schema_version == raw_demosaic_receipt_schema_version
           && receipt.source_raw_frame_schema_version == raw_frame_schema_version
           && receipt.black_subtraction_applied && receipt.white_level_normalization_applied
           && !receipt.dng_opcodes_applied;
}

[[nodiscard]] FusedRawFrameDevelopment develop_on_cpu(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawDevelopmentQuality quality,
    const RawHighlightRecoveryIntent highlight_recovery
) {
    const auto& descriptor = frame.descriptor;
    const Dimensions reconstruction_dimensions =
        preview_max_edge.has_value()
            ? proxy_dimensions(descriptor.active_dimensions, *preview_max_edge)
            : descriptor.active_dimensions;
    const bool area_preview = reconstruction_dimensions != descriptor.active_dimensions;
    const detail::BayerCfaSamplingPolicy cfa_sampling =
        highlight_recovery == RawHighlightRecoveryIntent::provider_default
            ? detail::editable_raw_cfa_sampling_policy(transform)
        : highlight_recovery == RawHighlightRecoveryIntent::aggressive
            ? detail::aggressive_highlight_repair_cfa_sampling_policy(transform)
            : detail::BayerCfaSamplingPolicy{};
    const auto area_sampling =
        area_preview ? std::optional<detail::BayerAreaSamplingGrid>(
                           detail::make_bayer_area_sampling_grid(frame, reconstruction_dimensions)
                       )
                     : std::nullopt;
    const Dimensions output_dimensions =
        oriented_dimensions(reconstruction_dimensions, descriptor.orientation);
    SceneLinearRgbFrame output;
    if (!area_preview) {
        const auto region = raw_pipeline_detail::prepare_raw_frame_region(
            frame,
            quality,
            detail::PreparedRawBayerDenoise{},
            GeometryPixelRect{
                .x = 0U,
                .y = 0U,
                .width = output_dimensions.width,
                .height = output_dimensions.height,
            }
        );
        output = raw_pipeline_detail::develop_raw_frame_region_cpu(
            frame,
            transform,
            highlight_recovery,
            region
        );
    } else {
        output = allocate_output(output_dimensions);

        detail::parallel_for_rows(
            output_dimensions.height,
            8U,
            [&frame,
             &transform,
             &output,
             reconstruction_dimensions,
             output_dimensions,
             area_sampling,
             cfa_sampling](const std::uint32_t first_row, const std::uint32_t last_row) {
                for (std::uint32_t output_y = first_row; output_y < last_row; ++output_y) {
                    for (std::uint32_t output_x = 0U; output_x < output_dimensions.width;
                         ++output_x) {
                        const auto [source_x, source_y] = source_coordinate(
                            output_x,
                            output_y,
                            reconstruction_dimensions,
                            frame.descriptor.orientation
                        );
                        const detail::CameraRgbSample camera = detail::area_camera_rgb_sample_at(
                            frame,
                            *area_sampling,
                            source_x,
                            source_y,
                            &transform,
                            cfa_sampling
                        );
                        const auto output_index =
                            (static_cast<std::size_t>(output_y) * output_dimensions.width
                             + output_x)
                            * 3U;
                        raw_pipeline_detail::write_raw_frame_transformed_pixel(
                            camera,
                            transform,
                            output.samples.data() + output_index
                        );
                    }
                }
            }
        );
    }

    FusedRawFrameDevelopment result{
        .scene_linear = std::move(output),
        .demosaic_receipt =
            area_preview
                ? make_area_demosaic_receipt(frame, transform)
                : raw_pipeline_detail::raw_frame_region_demosaic_receipt(frame, transform, quality),
        .backend = RawDevelopmentBackend::cpu,
        .highlight_recovery = highlight_recovery,
    };
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "fused CPU Bayer development produced an invalid scene-linear raster"
        );
    }
    return result;
}

} // namespace

std::string_view raw_development_backend_identity(const RawDevelopmentBackend backend) noexcept {
    switch (backend) {
    case RawDevelopmentBackend::cpu:
        return "shadow-fused-raw-cpu-v1;demosaic=plan-selected;"
               "sensor-highlight-policy=explicit";
    case RawDevelopmentBackend::metal:
        return "shadow-fused-raw-metal-v1;math=f32-precise;"
               "demosaic=plan-selected;sensor-highlight-policy=explicit";
    }
    return "shadow-fused-raw-unknown";
}

std::string_view
raw_highlight_treatment_identity(const RawHighlightRecoveryIntent intent) noexcept {
    switch (intent) {
    case RawHighlightRecoveryIntent::provider_default:
        return "sensor-highlights=cfa-opposed-point+cached-chrominance@20260826.1;"
               "recovery=local-opposed+cached-global-chrominance;"
               "headroom=physical-white-wb-fp32;"
               "clipped-highlight=cfa-opposed-physical-white-chrominance-v24";
    case RawHighlightRecoveryIntent::disabled:
        return "sensor-highlights=disabled";
    case RawHighlightRecoveryIntent::conservative:
        return "sensor-highlights=unsupported";
    case RawHighlightRecoveryIntent::aggressive:
        return "sensor-highlights=cfa-opposed-cached-chrominance-feathered@20260826.1;"
               "recovery=local-opposed+cached-global-chrominance+explicit-spatial-chroma;"
               "headroom=physical-white-wb-fp32;"
               "clipped-highlight=cfa-opposed-physical-white-chrominance-v24";
    }
    return "sensor-highlights=unknown";
}

bool raw_development_backend_available(const RawDevelopmentBackend backend) noexcept {
    switch (backend) {
    case RawDevelopmentBackend::cpu:
        return true;
    case RawDevelopmentBackend::metal:
        return detail::metal_raw_development_available();
    }
    return false;
}

RawDevelopmentBackendMode raw_development_backend_mode_from_environment() {
    const auto preference = detail::image_acceleration_preference_from_environment();
    if (!preference.has_value()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "SHADOW_IMAGE_ACCELERATION must be auto, cpu, or metal"
        );
    }
    if (*preference == detail::ImageAccelerationPreference::automatic) {
        return RawDevelopmentBackendMode::automatic;
    }
    if (*preference == detail::ImageAccelerationPreference::cpu) {
        return RawDevelopmentBackendMode::cpu;
    }
    return RawDevelopmentBackendMode::metal;
}

bool RawFrameLinearTransform::valid() const noexcept {
    bool non_zero = false;
    for (const double coefficient : camera_to_linear_srgb_d65) {
        if (!std::isfinite(coefficient)) {
            return false;
        }
        non_zero = non_zero || coefficient != 0.0;
    }
    for (const double neutral : camera_neutral) {
        if (!std::isfinite(neutral) || neutral <= 0.0) {
            return false;
        }
    }
    for (const double multiplier : cfa_white_balance) {
        if (!std::isfinite(multiplier) || multiplier <= 0.0) {
            return false;
        }
    }
    for (const float offset : opposed_highlight_chrominance_offsets) {
        if (!std::isfinite(offset)) {
            return false;
        }
    }
    return non_zero;
}

bool FusedRawFrameDevelopment::valid() const noexcept {
    const auto width = static_cast<std::uint64_t>(scene_linear.dimensions.width);
    const auto height = static_cast<std::uint64_t>(scene_linear.dimensions.height);
    const bool known_backend =
        backend == RawDevelopmentBackend::cpu || backend == RawDevelopmentBackend::metal;
    const bool known_highlight_treatment =
        highlight_recovery == RawHighlightRecoveryIntent::provider_default
        || highlight_recovery == RawHighlightRecoveryIntent::disabled
        || highlight_recovery == RawHighlightRecoveryIntent::aggressive;
    if (!known_backend || !known_highlight_treatment || width == 0U || height == 0U
        || !scene_linear.valid() || !valid_demosaic_receipt(demosaic_receipt)) {
        return false;
    }
    return true;
}

FusedRawFrameDevelopment develop_bayer_linear_srgb_f32_fused(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality
) {
    return develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        transform,
        preview_max_edge,
        raw_development_backend_mode_from_environment(),
        highlight_recovery,
        quality
    );
}

FusedRawFrameDevelopment develop_bayer_linear_srgb_f32_fused_with_backend(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawDevelopmentBackendMode backend_mode,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality
) {
    validate_request(frame, transform, preview_max_edge, quality, highlight_recovery);
    if (backend_mode != RawDevelopmentBackendMode::cpu) {
        auto attempt = detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            preview_max_edge,
            highlight_recovery,
            quality
        );
        if (attempt.development.has_value()) {
            return std::move(*attempt.development);
        }
        if (backend_mode == RawDevelopmentBackendMode::metal) {
            const std::string diagnostic = attempt.diagnostic.empty()
                                               ? "Metal RAW development is unavailable"
                                               : std::move(attempt.diagnostic);
            throw DecodeError(DecodeErrorCode::internal, 0, diagnostic);
        }
    }
    auto result = develop_on_cpu(frame, transform, preview_max_edge, quality, highlight_recovery);
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "fused RAW backend selection produced an invalid scene-linear raster"
        );
    }
    return result;
}

} // namespace shadow::image

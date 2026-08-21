#include "raw_frame_region_development.hpp"

#include <shadow/image/decoder_error.hpp>

#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace shadow::image::raw_pipeline_detail {

namespace {

[[nodiscard]] bool rect_inside(const GeometryPixelRect rect, const Dimensions dimensions) noexcept {
    return rect.width != 0U && rect.height != 0U && rect.x < dimensions.width
           && rect.y < dimensions.height && rect.width <= dimensions.width - rect.x
           && rect.height <= dimensions.height - rect.y;
}

[[nodiscard]] bool supported_orientation(const std::int32_t orientation) noexcept {
    return orientation == 0 || orientation == 3 || orientation == 5 || orientation == 6;
}

[[nodiscard]] GeometryPixelRect expanded_rect(
    const GeometryPixelRect rect,
    const std::uint32_t radius,
    const Dimensions bounds
) noexcept {
    const std::uint32_t left = std::min(rect.x, radius);
    const std::uint32_t top = std::min(rect.y, radius);
    const std::uint32_t right = std::min(bounds.width - (rect.x + rect.width), radius);
    const std::uint32_t bottom = std::min(bounds.height - (rect.y + rect.height), radius);
    return GeometryPixelRect{
        .x = rect.x - left,
        .y = rect.y - top,
        .width = rect.width + left + right,
        .height = rect.height + top + bottom,
    };
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> reconstruction_coordinate(
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const Dimensions reconstruction,
    const std::int32_t orientation
) noexcept {
    switch (orientation) {
    case 3:
        return {
            reconstruction.width - 1U - output_x,
            reconstruction.height - 1U - output_y,
        };
    case 5:
        return {reconstruction.width - 1U - output_y, output_x};
    case 6:
        return {output_y, reconstruction.height - 1U - output_x};
    case 0:
    default:
        return {output_x, output_y};
    }
}

[[nodiscard]] GeometryPixelRect reconstruction_rect_for_output(
    const GeometryPixelRect output,
    const Dimensions reconstruction,
    const std::int32_t orientation
) noexcept {
    const std::array corners{
        reconstruction_coordinate(output.x, output.y, reconstruction, orientation),
        reconstruction_coordinate(
            output.x + output.width - 1U,
            output.y,
            reconstruction,
            orientation
        ),
        reconstruction_coordinate(
            output.x,
            output.y + output.height - 1U,
            reconstruction,
            orientation
        ),
        reconstruction_coordinate(
            output.x + output.width - 1U,
            output.y + output.height - 1U,
            reconstruction,
            orientation
        ),
    };
    std::uint32_t minimum_x = corners.front().first;
    std::uint32_t maximum_x = corners.front().first;
    std::uint32_t minimum_y = corners.front().second;
    std::uint32_t maximum_y = corners.front().second;
    for (const auto [x, y] : corners) {
        minimum_x = std::min(minimum_x, x);
        maximum_x = std::max(maximum_x, x);
        minimum_y = std::min(minimum_y, y);
        maximum_y = std::max(maximum_y, y);
    }
    return GeometryPixelRect{
        .x = minimum_x,
        .y = minimum_y,
        .width = maximum_x - minimum_x + 1U,
        .height = maximum_y - minimum_y + 1U,
    };
}

[[nodiscard]] std::uint32_t required_demosaic_halo(const RawDevelopmentQuality quality) noexcept {
    // Edge-aware reconstruction asks directional-green estimates for neighbours, and those
    // estimates inspect two additional sensor sites. Three stored pixels is therefore the exact
    // conservative preimage radius. Bilinear reconstruction inspects one 3x3 neighbourhood.
    return quality == RawDevelopmentQuality::high ? 3U : 1U;
}

[[nodiscard]] std::uint32_t
required_denoise_halo(const detail::PreparedRawBayerDenoise& denoise) noexcept {
    switch (denoise.mode) {
    case RawBayerDenoiseMode::skipped:
        return 0U;
    case RawBayerDenoiseMode::cfa_bilateral_conservative_v1:
        return 2U;
    case RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1:
        return 4U;
    }
    return 0U;
}

[[nodiscard]] SceneLinearRgbFrame allocate_output(const Dimensions dimensions) {
    const std::uint64_t pixels = dimensions.pixel_count();
    if (pixels > std::numeric_limits<std::uint64_t>::max() / 3U) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "resident RAW region RGB sample count overflows"
        );
    }
    const std::uint64_t sample_count = pixels * 3U;
    if (sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || static_cast<std::uint64_t>(dimensions.width) * 3U
               > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "resident RAW region output exceeds the address space"
        );
    }
    SceneLinearRgbFrame output;
    output.dimensions = dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    output.samples.resize(static_cast<std::size_t>(sample_count));
    return output;
}

} // namespace

Dimensions oriented_raw_dimensions(
    const Dimensions reconstruction_dimensions,
    const std::int32_t orientation
) noexcept {
    return orientation == 5 || orientation == 6
               ? Dimensions{reconstruction_dimensions.height, reconstruction_dimensions.width}
               : reconstruction_dimensions;
}

PreparedRawFrameRegion::PreparedRawFrameRegion(
    const GeometryPixelRect requested_core,
    const GeometryPixelRect reconstruction_core,
    const GeometryPixelRect demosaic_sensor_preimage,
    const GeometryPixelRect denoise_sensor_preimage,
    const GeometryPixelRect optics_output_preimage,
    const std::uint32_t demosaic_halo,
    const std::uint32_t denoise_halo,
    const RawDemosaicAlgorithm algorithm,
    const Dimensions source_storage_dimensions,
    const Dimensions source_active_dimensions,
    const Margins source_active_margins,
    const std::int32_t source_orientation,
    const std::uint32_t source_schema_version,
    const RawFrameCfaLayout source_cfa_layout,
    const std::array<RawCfaColor, 4U> source_bayer_2x2,
    const RawDevelopmentQuality quality,
    const RawBayerDenoiseMode denoise_mode
) noexcept :
    requested_core_(requested_core), reconstruction_core_(reconstruction_core),
    demosaic_sensor_preimage_(demosaic_sensor_preimage),
    denoise_sensor_preimage_(denoise_sensor_preimage),
    optics_output_preimage_(optics_output_preimage), demosaic_halo_(demosaic_halo),
    denoise_halo_(denoise_halo), algorithm_(algorithm),
    source_storage_dimensions_(source_storage_dimensions),
    source_active_dimensions_(source_active_dimensions),
    source_active_margins_(source_active_margins), source_orientation_(source_orientation),
    source_schema_version_(source_schema_version), source_cfa_layout_(source_cfa_layout),
    source_bayer_2x2_(source_bayer_2x2), quality_(quality), denoise_mode_(denoise_mode) {}

GeometryPixelRect PreparedRawFrameRegion::requested_core() const noexcept {
    return requested_core_;
}

GeometryPixelRect PreparedRawFrameRegion::reconstruction_core() const noexcept {
    return reconstruction_core_;
}

GeometryPixelRect PreparedRawFrameRegion::demosaic_sensor_preimage() const noexcept {
    return demosaic_sensor_preimage_;
}

GeometryPixelRect PreparedRawFrameRegion::denoise_sensor_preimage() const noexcept {
    return denoise_sensor_preimage_;
}

GeometryPixelRect PreparedRawFrameRegion::optics_output_preimage() const noexcept {
    return optics_output_preimage_;
}

std::uint32_t PreparedRawFrameRegion::demosaic_halo() const noexcept {
    return demosaic_halo_;
}

std::uint32_t PreparedRawFrameRegion::denoise_halo() const noexcept {
    return denoise_halo_;
}

RawDemosaicAlgorithm PreparedRawFrameRegion::algorithm() const noexcept {
    return algorithm_;
}

bool PreparedRawFrameRegion::valid(
    const RawFrameDescriptor& descriptor,
    const Dimensions output_dimensions
) const noexcept {
    if (!supported_orientation(descriptor.orientation)
        || descriptor.storage_dimensions != source_storage_dimensions_
        || descriptor.active_dimensions != source_active_dimensions_
        || descriptor.active_margins != source_active_margins_
        || descriptor.orientation != source_orientation_
        || descriptor.schema_version != source_schema_version_
        || descriptor.cfa_layout != source_cfa_layout_ || descriptor.bayer_2x2 != source_bayer_2x2_
        || output_dimensions
               != oriented_raw_dimensions(descriptor.active_dimensions, descriptor.orientation)
        || !rect_inside(requested_core_, output_dimensions)) {
        return false;
    }
    const GeometryPixelRect expected_reconstruction = reconstruction_rect_for_output(
        requested_core_,
        descriptor.active_dimensions,
        descriptor.orientation
    );
    const GeometryPixelRect stored_core{
        .x = descriptor.active_margins.left + expected_reconstruction.x,
        .y = descriptor.active_margins.top + expected_reconstruction.y,
        .width = expected_reconstruction.width,
        .height = expected_reconstruction.height,
    };
    const std::uint32_t expected_demosaic_halo = required_demosaic_halo(quality_);
    const GeometryPixelRect expected_demosaic =
        expanded_rect(stored_core, expected_demosaic_halo, descriptor.storage_dimensions);
    detail::PreparedRawBayerDenoise denoise;
    denoise.mode = denoise_mode_;
    const std::uint32_t expected_denoise_halo = required_denoise_halo(denoise);
    const GeometryPixelRect expected_denoise =
        expanded_rect(expected_demosaic, expected_denoise_halo, descriptor.storage_dimensions);
    const RawDemosaicAlgorithm expected_algorithm = quality_ == RawDevelopmentQuality::high
                                                        ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                                                        : RawDemosaicAlgorithm::bayer_bilinear_v1;
    return reconstruction_core_ == expected_reconstruction
           && demosaic_sensor_preimage_ == expected_demosaic
           && denoise_sensor_preimage_ == expected_denoise
           && optics_output_preimage_ == requested_core_ && demosaic_halo_ == expected_demosaic_halo
           && denoise_halo_ == expected_denoise_halo && algorithm_ == expected_algorithm;
}

PreparedRawFrameRegion prepare_raw_frame_region(
    const RawFrame& frame,
    const RawDevelopmentQuality quality,
    const detail::PreparedRawBayerDenoise& denoise,
    const GeometryPixelRect requested_core
) {
    detail::validate_bayer_frame(frame, "resident RAW region preparation");
    if (!supported_orientation(frame.descriptor.orientation)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "resident RAW region does not support this source orientation"
        );
    }
    if (quality != RawDevelopmentQuality::fast && quality != RawDevelopmentQuality::balanced
        && quality != RawDevelopmentQuality::high) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "resident RAW region received an unknown quality tier"
        );
    }
    const Dimensions reconstruction = frame.descriptor.active_dimensions;
    const Dimensions output = oriented_raw_dimensions(reconstruction, frame.descriptor.orientation);
    if (!rect_inside(requested_core, output)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "resident RAW region core must be fully inside the oriented source"
        );
    }

    const GeometryPixelRect reconstruction_core = reconstruction_rect_for_output(
        requested_core,
        reconstruction,
        frame.descriptor.orientation
    );
    const GeometryPixelRect stored_core{
        .x = frame.descriptor.active_margins.left + reconstruction_core.x,
        .y = frame.descriptor.active_margins.top + reconstruction_core.y,
        .width = reconstruction_core.width,
        .height = reconstruction_core.height,
    };
    const std::uint32_t reconstruction_halo = required_demosaic_halo(quality);
    const GeometryPixelRect demosaic_preimage =
        expanded_rect(stored_core, reconstruction_halo, frame.descriptor.storage_dimensions);
    const std::uint32_t raw_denoise_halo = required_denoise_halo(denoise);
    const GeometryPixelRect denoise_preimage =
        expanded_rect(demosaic_preimage, raw_denoise_halo, frame.descriptor.storage_dimensions);
    PreparedRawFrameRegion result(
        requested_core,
        reconstruction_core,
        demosaic_preimage,
        denoise_preimage,
        requested_core,
        reconstruction_halo,
        raw_denoise_halo,
        quality == RawDevelopmentQuality::high ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                                               : RawDemosaicAlgorithm::bayer_bilinear_v1,
        frame.descriptor.storage_dimensions,
        frame.descriptor.active_dimensions,
        frame.descriptor.active_margins,
        frame.descriptor.orientation,
        frame.descriptor.schema_version,
        frame.descriptor.cfa_layout,
        frame.descriptor.bayer_2x2,
        quality,
        denoise.mode
    );
    if (!result.valid(frame.descriptor, output)) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "resident RAW region preparation produced an invalid dependency plan"
        );
    }
    return result;
}

void write_raw_frame_transformed_pixel(
    const detail::CameraRgbSample& camera,
    const RawFrameLinearTransform& transform,
    float* destination
) noexcept {
    const std::array<double, 3U> camera_values{
        camera.values[0],
        camera.values[1],
        camera.values[2],
    };
    // Once a physical white sits in a bright CFA footprint, that neutral-highlight RGB ratio is
    // no longer measured data. Neutralize that camera-RGB ratio before the camera matrix, where
    // LibRaw H=0 likewise establishes highlight colour; preserve the Bayer-weighted camera signal
    // and spatial structure. Doing this after the matrix creates a display-grey plateau whose
    // luminance does not follow the camera calibration.
    const double chroma_neutralization = std::clamp(
        static_cast<double>(camera.highlight_chroma_neutralization),
        0.0,
        1.0
    );
    std::array<double, 3U> adjusted_camera = camera_values;
    if (chroma_neutralization > 0.0) {
        const double camera_luminance = 0.25 * camera_values[0] + 0.5 * camera_values[1]
            + 0.25 * camera_values[2];
        for (auto& component : adjusted_camera) {
            component += chroma_neutralization * (camera_luminance - component);
        }
    }
    std::array<double, 3U> scene_linear{};
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            scene_linear[output] += transform.camera_to_linear_srgb_d65[output * 3U + input]
                                    * adjusted_camera[input];
        }
    }
    for (std::size_t output = 0U; output < 3U; ++output) {
        destination[output] = static_cast<float>(scene_linear[output]);
    }
}

SceneLinearRgbFrame develop_raw_frame_region_cpu(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const RawHighlightRecoveryIntent highlight_recovery,
    const PreparedRawFrameRegion& region
) {
    detail::validate_bayer_frame(frame, "resident RAW region development");
    if (!transform.valid()
        || (highlight_recovery != RawHighlightRecoveryIntent::provider_default
            && highlight_recovery != RawHighlightRecoveryIntent::disabled
            && highlight_recovery != RawHighlightRecoveryIntent::aggressive)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "resident RAW region requires a valid transform and highlight policy"
        );
    }

    // Keep black-subtracted CFA samples through white balance before demosaic. The editable source
    // retains WB-induced float headroom. The default uses the exact CFA footprint; opt-in
    // aggressive repair feathers only physical-white evidence before the same camera-domain
    // neutral pull. Neither route reconstructs colour or detail.
    const detail::BayerCfaSamplingPolicy cfa_sampling =
        highlight_recovery == RawHighlightRecoveryIntent::provider_default
            ? detail::editable_raw_cfa_sampling_policy(transform)
            : highlight_recovery == RawHighlightRecoveryIntent::aggressive
                ? detail::aggressive_highlight_repair_cfa_sampling_policy(transform)
                : detail::BayerCfaSamplingPolicy{};
    const Dimensions reconstruction = frame.descriptor.active_dimensions;
    const Dimensions output_dimensions =
        oriented_raw_dimensions(reconstruction, frame.descriptor.orientation);
    if (!region.valid(frame.descriptor, output_dimensions)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "resident RAW region development received an invalid dependency plan"
        );
    }

    SceneLinearRgbFrame output = allocate_output(
        Dimensions{
            region.requested_core().width,
            region.requested_core().height,
        }
    );
    detail::parallel_for_rows(
        region.requested_core().height,
        16U,
        [&](const std::uint32_t first_row, const std::uint32_t last_row) {
            for (std::uint32_t local_y = first_row; local_y < last_row; ++local_y) {
                const std::uint32_t output_y = region.requested_core().y + local_y;
                for (std::uint32_t local_x = 0U; local_x < region.requested_core().width;
                     ++local_x) {
                    const std::uint32_t output_x = region.requested_core().x + local_x;
                    const auto [source_x, source_y] = reconstruction_coordinate(
                        output_x,
                        output_y,
                        reconstruction,
                        frame.descriptor.orientation
                    );
                    const std::uint32_t raw_x = frame.descriptor.active_margins.left + source_x;
                    const std::uint32_t raw_y = frame.descriptor.active_margins.top + source_y;
                    const detail::CameraRgbSample camera =
                        region.algorithm() == RawDemosaicAlgorithm::bayer_edge_aware_v1
                            ? detail::edge_aware_camera_rgb_sample_at(
                                  frame,
                                  raw_x,
                                  raw_y,
                                  &transform,
                                  cfa_sampling
                              )
                            : detail::bilinear_camera_rgb_sample_at(
                                  frame,
                                  raw_x,
                                  raw_y,
                                  &transform,
                                  cfa_sampling
                              );
                    const std::size_t output_index =
                        (static_cast<std::size_t>(local_y) * region.requested_core().width
                         + local_x)
                        * 3U;
                    write_raw_frame_transformed_pixel(
                        camera,
                        transform,
                        output.samples.data() + output_index
                    );
                }
            }
        }
    );
    return output;
}

RawDemosaicReceipt raw_frame_region_demosaic_receipt(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const RawDevelopmentQuality quality
) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = quality == RawDevelopmentQuality::high
                         ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                         : RawDemosaicAlgorithm::bayer_bilinear_v1,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = transform.apply_cfa_white_balance,
        .dng_opcodes_applied = false,
    };
}

} // namespace shadow::image::raw_pipeline_detail

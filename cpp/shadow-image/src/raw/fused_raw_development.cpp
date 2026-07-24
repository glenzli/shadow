#include <shadow/image/fused_raw_development.hpp>

#include "bayer_sampling.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace shadow::image {

namespace {

using CameraRgb = detail::CameraRgb;

void validate_request(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
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
}

[[nodiscard]] Dimensions oriented_dimensions(
    const Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    return orientation == 5 || orientation == 6
        ? Dimensions{dimensions.height, dimensions.width}
        : dimensions;
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

[[nodiscard]] std::uint16_t quantize_linear(const double value) noexcept {
    return static_cast<std::uint16_t>(
        std::lround(std::clamp(value, 0.0, 1.0) * 65'535.0)
    );
}

void write_transformed_pixel(
    const CameraRgb& camera,
    const RawFrameLinearTransform& transform,
    std::uint16_t* destination
) noexcept {
    for (std::size_t output = 0U; output < 3U; ++output) {
        double value = 0.0;
        for (std::size_t input = 0U; input < 3U; ++input) {
            value += transform.camera_to_linear_srgb_d65[output * 3U + input]
                * static_cast<double>(camera[input]);
        }
        destination[output] = quantize_linear(value);
    }
}

[[nodiscard]] PixelBuffer allocate_output(const Dimensions dimensions) {
    const auto sample_count = static_cast<std::uint64_t>(dimensions.width)
        * dimensions.height * 3U;
    if (sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "fused Bayer development output exceeds the address space"
        );
    }
    PixelBuffer output;
    output.dimensions = dimensions;
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes =
        static_cast<std::size_t>(dimensions.width) * 3U * sizeof(std::uint16_t);
    output.primaries = RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = RgbTransferFunction::linear;
    output.reference = RgbBufferReference::processed_raw;
    output.samples.resize(static_cast<std::size_t>(sample_count));
    return output;
}

[[nodiscard]] RawDemosaicReceipt make_demosaic_receipt(
    const RawFrame& frame,
    const RawDemosaicAlgorithm algorithm
) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = algorithm,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };
}

[[nodiscard]] bool valid_demosaic_receipt(const RawDemosaicReceipt& receipt) noexcept {
    return receipt.schema_version == raw_demosaic_receipt_schema_version
        && receipt.source_raw_frame_schema_version == raw_frame_schema_version
        && receipt.black_subtraction_applied
        && receipt.white_level_normalization_applied
        && !receipt.white_balance_applied
        && !receipt.dng_opcodes_applied;
}

} // namespace

bool RawFrameLinearTransform::valid() const noexcept {
    bool non_zero = false;
    for (const double coefficient : camera_to_linear_srgb_d65) {
        if (!std::isfinite(coefficient)) {
            return false;
        }
        non_zero = non_zero || coefficient != 0.0;
    }
    return non_zero;
}

bool FusedRawFrameDevelopment::valid() const noexcept {
    const auto width = static_cast<std::uint64_t>(pixels.dimensions.width);
    const auto height = static_cast<std::uint64_t>(pixels.dimensions.height);
    if (width == 0U || height == 0U || pixels.bits_per_channel != 16U
        || pixels.channels != 3U
        || pixels.row_stride_bytes != width * 3U * sizeof(std::uint16_t)
        || pixels.primaries != RgbPrimaries::srgb_rec709_d65
        || pixels.transfer_function != RgbTransferFunction::linear
        || pixels.reference != RgbBufferReference::processed_raw
        || !valid_demosaic_receipt(demosaic_receipt)) {
        return false;
    }
    const auto sample_count = width * height * 3U;
    return sample_count <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        && pixels.samples.size() == static_cast<std::size_t>(sample_count);
}

FusedRawFrameDevelopment develop_bayer_linear_srgb_u16_fused(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
) {
    validate_request(frame, transform, preview_max_edge);

    const auto& descriptor = frame.descriptor;
    const Dimensions reconstruction_dimensions = preview_max_edge.has_value()
        ? proxy_dimensions(descriptor.active_dimensions, *preview_max_edge)
        : descriptor.active_dimensions;
    const bool area_preview = reconstruction_dimensions != descriptor.active_dimensions;
    const auto area_sampling = area_preview
        ? std::optional<detail::BayerAreaSamplingGrid>(
            detail::make_bayer_area_sampling_grid(frame, reconstruction_dimensions)
        )
        : std::nullopt;
    const Dimensions output_dimensions = oriented_dimensions(
        reconstruction_dimensions,
        descriptor.orientation
    );
    PixelBuffer output = allocate_output(output_dimensions);

    detail::parallel_for_rows(
        output_dimensions.height,
        area_preview ? 8U : 16U,
        [&frame,
         &transform,
         &output,
         reconstruction_dimensions,
         output_dimensions,
         area_preview,
         area_sampling](
            const std::uint32_t first_row,
            const std::uint32_t last_row
        ) {
            for (std::uint32_t output_y = first_row; output_y < last_row; ++output_y) {
                for (std::uint32_t output_x = 0U;
                     output_x < output_dimensions.width;
                     ++output_x) {
                    const auto [source_x, source_y] = source_coordinate(
                        output_x,
                        output_y,
                        reconstruction_dimensions,
                        frame.descriptor.orientation
                    );
                    const CameraRgb camera = area_preview
                        ? detail::area_camera_rgb_at(
                            frame,
                            *area_sampling,
                            source_x,
                            source_y
                        )
                        : detail::bilinear_camera_rgb_at(
                            frame,
                            frame.descriptor.active_margins.left + source_x,
                            frame.descriptor.active_margins.top + source_y
                        );
                    const auto output_index =
                        (static_cast<std::size_t>(output_y) * output_dimensions.width + output_x)
                        * 3U;
                    write_transformed_pixel(
                        camera,
                        transform,
                        output.samples.data() + output_index
                    );
                }
            }
        }
    );

    FusedRawFrameDevelopment result{
        .pixels = std::move(output),
        .demosaic_receipt = make_demosaic_receipt(
            frame,
            area_preview
                ? RawDemosaicAlgorithm::bayer_area_preview_v1
                : RawDemosaicAlgorithm::bayer_bilinear_v1
        ),
    };
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "fused Bayer development produced an invalid scene-linear raster"
        );
    }
    return result;
}

} // namespace shadow::image

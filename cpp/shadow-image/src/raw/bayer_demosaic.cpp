#include <shadow/image/raw_development.hpp>

#include "bayer_sampling.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <cmath>
#include <limits>

namespace shadow::image {

namespace {

[[nodiscard]] LinearCameraRgbFrame allocate_camera_rgb(
    const Dimensions dimensions,
    const std::uint32_t source_schema,
    const RawDemosaicAlgorithm algorithm
) {
    const auto output_samples = static_cast<std::uint64_t>(dimensions.width)
        * dimensions.height * 3U;
    if (output_samples > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Bayer demosaic output exceeds the address space"
        );
    }
    LinearCameraRgbFrame output;
    output.dimensions = dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    output.samples.resize(static_cast<std::size_t>(output_samples));
    output.receipt = RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = source_schema,
        .algorithm = algorithm,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };
    return output;
}

} // namespace

bool LinearCameraRgbFrame::valid() const noexcept {
    const auto width = static_cast<std::uint64_t>(dimensions.width);
    const auto height = static_cast<std::uint64_t>(dimensions.height);
    if (width == 0U || height == 0U || row_stride_bytes != width * 3U * sizeof(float)) {
        return false;
    }
    const auto sample_count = width * height * 3U;
    if (
        sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || samples.size() != static_cast<std::size_t>(sample_count)
        || receipt.schema_version != raw_demosaic_receipt_schema_version
        || receipt.source_raw_frame_schema_version != raw_frame_schema_version
        || !receipt.black_subtraction_applied || !receipt.white_level_normalization_applied
        || receipt.white_balance_applied || receipt.dng_opcodes_applied
    ) {
        return false;
    }
    for (const auto sample : samples) {
        if (!std::isfinite(sample)) {
            return false;
        }
    }
    return true;
}

LinearCameraRgbFrame demosaic_bayer_bilinear(const RawFrame& frame) {
    detail::validate_bayer_frame(frame, "Bayer demosaic");

    const auto& descriptor = frame.descriptor;
    const auto active_width = descriptor.active_dimensions.width;
    const auto active_height = descriptor.active_dimensions.height;
    const auto left = descriptor.active_margins.left;
    const auto top = descriptor.active_margins.top;
    LinearCameraRgbFrame output = allocate_camera_rgb(
        descriptor.active_dimensions,
        descriptor.schema_version,
        RawDemosaicAlgorithm::bayer_bilinear_v1
    );

    detail::parallel_for_rows(
        active_height,
        16U,
        [&frame, &output, active_width, left, top](
            const std::uint32_t first_row,
            const std::uint32_t last_row
        ) {
            for (std::uint32_t output_y = first_row; output_y < last_row; ++output_y) {
                const auto raw_y = top + output_y;
                for (std::uint32_t output_x = 0U; output_x < active_width; ++output_x) {
                    const auto raw_x = left + output_x;
                    const auto rgb = detail::bilinear_camera_rgb_at(frame, raw_x, raw_y);
                    const auto output_index =
                        (static_cast<std::size_t>(output_y) * active_width + output_x) * 3U;
                    output.samples[output_index] = rgb[0];
                    output.samples[output_index + 1U] = rgb[1];
                    output.samples[output_index + 2U] = rgb[2];
                }
            }
        }
    );

    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Bayer demosaic produced an invalid camera-linear frame"
        );
    }
    return output;
}

LinearCameraRgbFrame demosaic_bayer_preview(
    const RawFrame& frame,
    const std::uint32_t max_edge
) {
    detail::validate_bayer_frame(frame, "Bayer preview demosaic");
    if (max_edge == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer preview demosaic max edge must be non-zero"
        );
    }
    const auto& descriptor = frame.descriptor;
    const Dimensions target = proxy_dimensions(descriptor.active_dimensions, max_edge);
    if (target == descriptor.active_dimensions) {
        return demosaic_bayer_bilinear(frame);
    }

    LinearCameraRgbFrame output = allocate_camera_rgb(
        target,
        descriptor.schema_version,
        RawDemosaicAlgorithm::bayer_area_preview_v1
    );
    const auto sampling = detail::make_bayer_area_sampling_grid(frame, target);

    detail::parallel_for_rows(
        target.height,
        8U,
        [&frame, &output, target, sampling](
            const std::uint32_t first_row,
            const std::uint32_t last_row
        ) {
            for (std::uint32_t output_y = first_row; output_y < last_row; ++output_y) {
                for (std::uint32_t output_x = 0U; output_x < target.width; ++output_x) {
                    const auto rgb = detail::area_camera_rgb_at(
                        frame,
                        sampling,
                        output_x,
                        output_y
                    );
                    const auto output_index =
                        (static_cast<std::size_t>(output_y) * target.width + output_x) * 3U;
                    output.samples[output_index] = rgb[0];
                    output.samples[output_index + 1U] = rgb[1];
                    output.samples[output_index + 2U] = rgb[2];
                }
            }
        }
    );

    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Bayer preview demosaic produced an invalid camera-linear frame"
        );
    }
    return output;
}

} // namespace shadow::image

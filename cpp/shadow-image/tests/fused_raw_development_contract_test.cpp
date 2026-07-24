#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace {

// The optimized path is required to be a storage optimization, not a silent color change.
int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::RawFrame synthetic_frame(const std::int32_t orientation) {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "fused-contract";
    descriptor.provider_version = "v1";
    descriptor.storage_dimensions = {8U, 6U};
    descriptor.active_dimensions = {6U, 4U};
    descriptor.active_margins = {
        .left = 1U,
        .top = 1U,
        .right = 1U,
        .bottom = 1U,
    };
    descriptor.orientation = orientation;
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.cfa_pattern = "RGGB";
    descriptor.bits_per_sample = 12U;
    descriptor.black_levels = {10U, 20U, 30U, 40U};
    descriptor.white_levels = {1'010U, 1'020U, 1'030U, 1'040U};
    descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.25};
    frame.samples.resize(8U * 6U);
    for (std::uint32_t y = 0U; y < 6U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const std::uint32_t signal =
                80U + x * 17U + y * 29U + static_cast<std::uint32_t>(site) * 23U;
            frame.samples[static_cast<std::size_t>(y) * 8U + x] =
                static_cast<std::uint16_t>(descriptor.black_levels[site] + signal);
        }
    }
    return frame;
}

[[nodiscard]] image::Dimensions oriented_dimensions(
    const image::Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    return orientation == 5 || orientation == 6
        ? image::Dimensions{dimensions.height, dimensions.width}
        : dimensions;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> source_coordinate(
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const image::Dimensions source,
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

[[nodiscard]] std::uint16_t quantize(const double value) noexcept {
    return static_cast<std::uint16_t>(
        std::lround(std::clamp(value, 0.0, 1.0) * 65'535.0)
    );
}

[[nodiscard]] image::PixelBuffer reference_two_stage(
    const image::RawFrame& frame,
    const image::RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> max_edge
) {
    const image::LinearCameraRgbFrame camera = max_edge.has_value()
        ? image::demosaic_bayer_preview(frame, *max_edge)
        : image::demosaic_bayer_bilinear(frame);
    image::PixelBuffer output;
    output.dimensions = oriented_dimensions(camera.dimensions, frame.descriptor.orientation);
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes =
        static_cast<std::size_t>(output.dimensions.width) * 3U * sizeof(std::uint16_t);
    output.primaries = image::RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = image::RgbTransferFunction::linear;
    output.reference = image::RgbBufferReference::processed_raw;
    output.samples.resize(
        static_cast<std::size_t>(output.dimensions.width) * output.dimensions.height * 3U
    );
    for (std::uint32_t y = 0U; y < output.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < output.dimensions.width; ++x) {
            const auto [source_x, source_y] = source_coordinate(
                x,
                y,
                camera.dimensions,
                frame.descriptor.orientation
            );
            const auto source_index =
                (static_cast<std::size_t>(source_y) * camera.dimensions.width + source_x) * 3U;
            const auto output_index =
                (static_cast<std::size_t>(y) * output.dimensions.width + x) * 3U;
            for (std::size_t output_channel = 0U; output_channel < 3U; ++output_channel) {
                double value = 0.0;
                for (std::size_t input_channel = 0U; input_channel < 3U; ++input_channel) {
                    value += transform.camera_to_linear_srgb_d65[
                        output_channel * 3U + input_channel
                    ] * static_cast<double>(camera.samples[source_index + input_channel]);
                }
                output.samples[output_index + output_channel] = quantize(value);
            }
        }
    }
    return output;
}

void full_resolution_matches_reference_for_every_supported_orientation() {
    // Generic route example: provider camera matrix with AsShotNeutral WB already folded into
    // its columns by the caller.
    const image::RawFrameLinearTransform generic_transform{{
        1.72, -0.18, -0.04,
        -0.11, 1.08, 0.03,
        0.02, -0.31, 3.76,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto expected = reference_two_stage(frame, generic_transform, std::nullopt);
        const auto actual = image::develop_bayer_linear_srgb_u16_fused(
            frame,
            generic_transform
        );
        expect(actual.valid(), "full fused result has a complete typed contract");
        expect(
            actual.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_bilinear_v1,
            "full fused result records bilinear reconstruction"
        );
        expect(
            actual.pixels.dimensions == expected.dimensions
                && actual.pixels.samples == expected.samples,
            "full fused pixels exactly match the float reference path after orientation"
        );
    }
}

void area_preview_matches_reference_for_every_supported_orientation() {
    // DCP route example: compile_dcp_color_transform() produces this same row-major matrix with
    // white balance and BaselineExposureOffset already included.
    const image::RawFrameLinearTransform dcp_transform{{
        1.31, -0.27, 0.08,
        -0.06, 1.14, -0.03,
        0.04, -0.22, 1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto expected = reference_two_stage(frame, dcp_transform, 3U);
        const auto actual = image::develop_bayer_linear_srgb_u16_fused(
            frame,
            dcp_transform,
            3U
        );
        expect(actual.valid(), "preview fused result has a complete typed contract");
        expect(
            actual.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
            "downscaled fused result records CFA-aware area integration"
        );
        expect(
            actual.pixels.dimensions == expected.dimensions
                && actual.pixels.samples == expected.samples,
            "preview fused pixels exactly match the float reference path after orientation"
        );
    }
}

void invalid_inputs_fail_closed() {
    const image::RawFrameLinearTransform identity{{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
    auto unsupported_orientation = synthetic_frame(1);
    try {
        static_cast<void>(image::develop_bayer_linear_srgb_u16_fused(
            unsupported_orientation,
            identity
        ));
        expect(false, "unsupported orientation is rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "unsupported orientation returns a typed layout error"
        );
    }

    try {
        static_cast<void>(image::develop_bayer_linear_srgb_u16_fused(
            synthetic_frame(0),
            image::RawFrameLinearTransform{},
            0U
        ));
        expect(false, "invalid transform and zero preview edge are rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "invalid fused request returns a typed request error"
        );
    }
}

} // namespace

int main() {
    full_resolution_matches_reference_for_every_supported_orientation();
    area_preview_matches_reference_for_every_supported_orientation();
    invalid_inputs_fail_closed();
    return failures == 0 ? 0 : 1;
}

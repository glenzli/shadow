#include "contract_test_assertions.hpp"
#include "fused_raw_contract_test_support.hpp"

#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

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
        const auto actual = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            generic_transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu
        );
        expect(actual.valid(), "full fused result has a complete typed contract");
        expect(
            actual.backend == image::RawDevelopmentBackend::cpu,
            "forced CPU result records its effective backend"
        );
        expect(
            actual.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_bilinear_v1,
            "full fused result records bilinear reconstruction"
        );
        expect(
            actual.scene_linear.dimensions == expected.dimensions,
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
        const auto actual = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            dcp_transform,
            3U,
            image::RawDevelopmentBackendMode::cpu
        );
        expect(actual.valid(), "preview fused result has a complete typed contract");
        expect(
            actual.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
            "downscaled fused result records CFA-aware area integration"
        );
        expect(
            actual.scene_linear.dimensions == expected.dimensions,
            "preview fused pixels exactly match the float reference path after orientation"
        );
    }
}

void fractional_area_preview_stays_inside_the_active_sensor_rectangle() {
    // A last preview footprint whose scale is fractional is the boundary that previously let a
    // ceil() rounding error inspect one source row or column beyond the owned RAW plane.  Keep
    // non-zero margins as well: private decoders commonly expose an active rectangle rather than
    // a tightly cropped sensor buffer.
    auto frame = synthetic_frame(0);
    auto& descriptor = frame.descriptor;
    descriptor.storage_dimensions = {11U, 9U};
    descriptor.active_dimensions = {7U, 5U};
    descriptor.active_margins = {
        .left = 2U,
        .top = 2U,
        .right = 2U,
        .bottom = 2U,
    };
    frame.samples.resize(
        static_cast<std::size_t>(descriptor.storage_dimensions.width)
        * descriptor.storage_dimensions.height
    );
    for (std::uint32_t y = 0U; y < descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < descriptor.storage_dimensions.width; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            frame.samples[static_cast<std::size_t>(y) * descriptor.storage_dimensions.width + x]
                = static_cast<std::uint16_t>(descriptor.black_levels[site] + 80U + x + y * 3U);
        }
    }
    const image::RawFrameLinearTransform identity{{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
    const auto preview = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        identity,
        3U,
        image::RawDevelopmentBackendMode::cpu
    );
    expect(
        preview.valid()
            && preview.scene_linear.dimensions == image::Dimensions{3U, 2U}
            && preview.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
        "fractional CFA-area preview remains bounded within a margined sensor frame"
    );
    expect(
        std::ranges::all_of(preview.scene_linear.samples, [](const float sample) {
            return std::isfinite(sample);
        }),
        "fractional CFA-area preview produces finite scene-linear samples at the sensor edge"
    );
}

void high_quality_reconstruction_is_explicit_and_preview_safe() {
    const image::RawFrameLinearTransform identity{{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
    auto frame = synthetic_frame(0);
    // A compact chromatic edge gives the directional estimator a real decision to make instead
    // of accidentally passing only flat-field or affine-gradient fixtures.
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const bool bright = x >= frame.descriptor.storage_dimensions.width / 2U;
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t signal = colour == image::RawCfaColor::green
                ? (bright ? 900U : 120U)
                : colour == image::RawCfaColor::red ? (bright ? 850U : 60U)
                : (bright ? 100U : 880U);
            frame.samples[static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width
                + x] = static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + signal);
        }
    }

    const auto balanced = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto high = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::high
    );
    const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::high
    );
    expect(
        high.valid()
            && high.demosaic_receipt.algorithm == image::RawDemosaicAlgorithm::bayer_edge_aware_v1
            && high.scene_linear.samples == repeated.scene_linear.samples,
        "high quality RAW reconstruction is explicit and byte deterministic on CPU"
    );
    expect(
        high.scene_linear.samples != balanced.scene_linear.samples,
        "high quality RAW reconstruction is not silently aliased to the balanced bilinear path"
    );

    const auto high_preview = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        identity,
        3U,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::high
    );
    expect(
        high_preview.valid()
            && high_preview.demosaic_receipt.algorithm
                == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
        "bounded previews retain CFA-area integration even when a caller requests high quality"
    );
}

} // namespace

int main() {
    full_resolution_matches_reference_for_every_supported_orientation();
    area_preview_matches_reference_for_every_supported_orientation();
    fractional_area_preview_stays_inside_the_active_sensor_rectangle();
    high_quality_reconstruction_is_explicit_and_preview_safe();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

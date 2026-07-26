#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
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

void metal_full_resolution_stays_within_the_linear_u16_contract() {
    expect(
        image::raw_development_backend_identity(image::RawDevelopmentBackend::cpu)
            != image::raw_development_backend_identity(image::RawDevelopmentBackend::metal),
        "CPU and Metal development identities remain distinct"
    );
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for this validation run but no Metal backend is available"
        );
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31, -0.27, 0.08,
        -0.06, 1.14, -0.03,
        0.04, -0.22, 1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu
        );
        const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        expect(metal.valid(), "Metal full result has a complete typed contract");
        expect(
            metal.backend == image::RawDevelopmentBackend::metal,
            "forced Metal result records its effective backend"
        );
        expect(
            metal.scene_linear.dimensions == cpu.scene_linear.dimensions
                && metal.scene_linear.samples.size() == cpu.scene_linear.samples.size(),
            "Metal preserves CPU dimensions and packed sample count"
        );
        expect(
            metal.scene_linear.samples == repeated.scene_linear.samples,
            "repeated Metal development is byte deterministic"
        );

        std::vector<float> differences;
        differences.reserve(cpu.scene_linear.samples.size());
        double total_difference = 0.0;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
            const float difference = std::abs(
                cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]
            );
            differences.push_back(difference);
            total_difference += difference;
        }
        std::sort(differences.begin(), differences.end());
        const auto p99_index = differences.empty()
            ? 0U : (differences.size() - 1U) * 99U / 100U;
        const auto maximum = differences.empty() ? 0U : differences.back();
        const auto p99 = differences.empty() ? 0U : differences[p99_index];
        const double mean = differences.empty()
            ? 0.0
            : static_cast<double>(total_difference)
                / static_cast<double>(differences.size());
        expect(maximum <= 4.0e-5F, "Metal maximum error stays within fp32 reconstruction tolerance");
        expect(p99 <= 2.0e-5F, "Metal p99 error stays within fp32 reconstruction tolerance");
        expect(mean <= 1.0e-6, "Metal mean error stays within fp32 reconstruction tolerance");
    }
}

void metal_area_preview_preserves_the_cfa_footprint_contract() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for area-preview validation but no Metal backend is available"
        );
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31, -0.27, 0.08,
        -0.06, 1.14, -0.03,
        0.04, -0.22, 1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::cpu
        );
        const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::metal
        );
        const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::metal
        );
        expect(
            metal.valid()
                && metal.backend == image::RawDevelopmentBackend::metal
                && metal.demosaic_receipt.algorithm
                    == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
            "Metal area preview retains the typed CFA-footprint receipt"
        );
        expect(
            metal.scene_linear.dimensions == cpu.scene_linear.dimensions
                && metal.scene_linear.samples.size() == cpu.scene_linear.samples.size(),
            "Metal area preview preserves CPU output dimensions and packing"
        );
        expect(
            metal.scene_linear.samples == repeated.scene_linear.samples,
            "Metal area preview is byte deterministic"
        );
        float maximum_error = 0.0F;
        double total_error = 0.0;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
            const float error = std::abs(
                cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]
            );
            maximum_error = std::max(maximum_error, error);
            total_error += error;
        }
        const double mean_error = cpu.scene_linear.samples.empty()
            ? 0.0
            : static_cast<double>(total_error) / static_cast<double>(cpu.scene_linear.samples.size());
        expect(maximum_error <= 4.0e-5F, "Metal area preview stays within fp32 CPU tolerance");
        expect(mean_error <= 1.0e-5, "Metal area preview stays within fp32 mean tolerance");
    }
}

[[nodiscard]] image::RawFrame sensor_clipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            // Red and blue are at sensor white, while green remains close enough to make the
            // old independent u16 clipping produce a magenta false highlight after a camera
            // matrix. This models the clipped-sun failure seen in real CR3 files.
            frame.samples[static_cast<std::size_t>(y)
                * frame.descriptor.storage_dimensions.width + x] = static_cast<std::uint16_t>(
                colour == image::RawCfaColor::green
                    ? 980U : frame.descriptor.white_levels[site]
            );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame single_channel_clipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            frame.samples[static_cast<std::size_t>(y)
                * frame.descriptor.storage_dimensions.width + x] = static_cast<std::uint16_t>(
                colour == image::RawCfaColor::red
                    ? frame.descriptor.white_levels[site] : 970U
            );
        }
    }
    return frame;
}

void sensor_clipped_highlights_are_neutral_before_u16_clipping() {
    const image::RawFrameLinearTransform transform{{
        1.60, -0.40, 0.00,
        0.00, 0.85, 0.00,
        0.10, -0.20, 1.50,
    }};
    for (const auto max_edge : {
             std::optional<std::uint32_t>{},
             std::optional<std::uint32_t>{3U},
         }) {
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            sensor_clipped_frame(),
            transform,
            max_edge,
            image::RawDevelopmentBackendMode::cpu
        );
        expect(
            cpu.highlight_recovery == image::RawHighlightRecoveryIntent::provider_default,
            "default fused development records sensor-highlight neutralization"
        );
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); index += 3U) {
            const auto red = cpu.scene_linear.samples[index];
            const auto green = cpu.scene_linear.samples[index + 1U];
            const auto blue = cpu.scene_linear.samples[index + 2U];
            const auto min_channel = std::min({red, green, blue});
            const auto max_channel = std::max({red, green, blue});
            expect(
                min_channel > 1.0F && max_channel - min_channel <= 1.0e-4F,
                "sensor-clipped highlights carry neutral scene-linear luminance without a hue"
            );
        }
    }

    const auto disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    expect(
        disabled.valid()
            && disabled.highlight_recovery == image::RawHighlightRecoveryIntent::disabled
            && image::raw_highlight_treatment_identity(disabled.highlight_recovery)
                == "sensor-highlights=disabled",
        "disabled highlight treatment remains explicit in the fused result"
    );
    bool disabled_preserves_channel_difference = false;
    for (std::size_t index = 0U; index < disabled.scene_linear.samples.size(); index += 3U) {
        const auto red = disabled.scene_linear.samples[index];
        const auto green = disabled.scene_linear.samples[index + 1U];
        const auto blue = disabled.scene_linear.samples[index + 2U];
        disabled_preserves_channel_difference =
            disabled_preserves_channel_difference
            || std::min({red, green, blue}) + 1.0e-3F < std::max({red, green, blue});
    }
    expect(
        disabled_preserves_channel_difference,
        "disabled highlight treatment does not silently neutralize clipped sensor colours"
    );

    const auto one_channel = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        single_channel_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    for (std::size_t index = 0U; index < one_channel.scene_linear.samples.size(); index += 3U) {
        const auto red = one_channel.scene_linear.samples[index];
        const auto green = one_channel.scene_linear.samples[index + 1U];
        const auto blue = one_channel.scene_linear.samples[index + 2U];
        const auto min_channel = std::min({red, green, blue});
        const auto max_channel = std::max({red, green, blue});
        expect(
            min_channel > 0.5F && max_channel - min_channel <= 0.25F,
            "a single clipped CFA colour with near-white companions stays bounded before output mapping"
        );
    }

    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }
    const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal
    );
    expect(
        metal.scene_linear.samples == cpu.scene_linear.samples,
        "Metal applies the same sensor-highlight neutralization as CPU"
    );

    const auto disabled_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal,
        image::RawHighlightRecoveryIntent::disabled
    );
    expect(
        disabled_metal.highlight_recovery == image::RawHighlightRecoveryIntent::disabled,
        "Metal records disabled sensor-highlight treatment"
    );
    float maximum_disabled_difference = 0.0F;
    for (std::size_t index = 0U; index < disabled.scene_linear.samples.size(); ++index) {
        maximum_disabled_difference = std::max(
            maximum_disabled_difference,
            std::abs(
                disabled.scene_linear.samples[index]
                - disabled_metal.scene_linear.samples[index]
            )
        );
    }
    expect(
        maximum_disabled_difference <= 4.0e-5F,
        "Metal disabled-highlight output stays within fp32 CPU tolerance"
    );
}

void invalid_inputs_fail_closed() {
    const image::RawFrameLinearTransform identity{{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
    auto unsupported_orientation = synthetic_frame(1);
    try {
        static_cast<void>(image::develop_bayer_linear_srgb_f32_fused(
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
        static_cast<void>(image::develop_bayer_linear_srgb_f32_fused(
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

    try {
        static_cast<void>(image::develop_bayer_linear_srgb_f32_fused_with_backend(
            synthetic_frame(0),
            identity,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::conservative
        ));
        expect(false, "unimplemented highlight reconstruction is rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "unimplemented highlight reconstruction fails with a typed unsupported error"
        );
    }

    for (const image::Dimensions dimensions : {
             image::Dimensions{1U, 4U},
             image::Dimensions{4U, 1U},
         }) {
        auto degenerate = synthetic_frame(0);
        degenerate.descriptor.storage_dimensions = dimensions;
        degenerate.descriptor.active_dimensions = dimensions;
        degenerate.descriptor.active_margins = {};
        degenerate.samples.resize(
            static_cast<std::size_t>(dimensions.width) * dimensions.height
        );
        for (const auto backend : {
                 image::RawDevelopmentBackendMode::cpu,
                 image::RawDevelopmentBackendMode::metal,
             }) {
            try {
                static_cast<void>(image::develop_bayer_linear_srgb_f32_fused_with_backend(
                    degenerate,
                    identity,
                    std::nullopt,
                    backend
                ));
                expect(false, "degenerate Bayer storage is rejected before backend selection");
            } catch (const image::DecodeError& error) {
                expect(
                    error.code() == image::DecodeErrorCode::unsupported_layout,
                    "CPU and Metal reject degenerate Bayer storage with the same typed error"
                );
            }
        }
    }
}

} // namespace

int main() {
    full_resolution_matches_reference_for_every_supported_orientation();
    area_preview_matches_reference_for_every_supported_orientation();
    high_quality_reconstruction_is_explicit_and_preview_safe();
    metal_full_resolution_stays_within_the_linear_u16_contract();
    metal_area_preview_preserves_the_cfa_footprint_contract();
    sensor_clipped_highlights_are_neutral_before_u16_clipping();
    invalid_inputs_fail_closed();
    return failures == 0 ? 0 : 1;
}

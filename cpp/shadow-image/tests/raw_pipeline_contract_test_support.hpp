#pragma once

#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::RawDevelopmentCapabilities raw_capabilities() {
    auto capabilities = image::RawDevelopmentCapabilities{};
    capabilities.schema_version = image::raw_development_capabilities_schema_version;
    capabilities.available = true;
    capabilities.raw_frame = true;
    capabilities.supported_intents =
        image::raw_development_intent_mask(image::RawDevelopmentIntent::preview)
        | image::raw_development_intent_mask(image::RawDevelopmentIntent::detail)
        | image::raw_development_intent_mask(image::RawDevelopmentIntent::export_image);
    capabilities.supported_qualities =
        image::raw_development_quality_mask(image::RawDevelopmentQuality::balanced);
    capabilities.supported_dng_opcode_policies =
        image::dng_opcode_policy_mask(image::DngOpcodePolicy::provider_default);
    capabilities.supported_noise_reduction_intents =
        image::raw_noise_reduction_intent_mask(image::RawNoiseReductionIntent::provider_default);
    capabilities.supported_highlight_recovery_intents =
        image::raw_highlight_recovery_intent_mask(
            image::RawHighlightRecoveryIntent::provider_default
        );
    return capabilities;
}

[[nodiscard]] image::RawFrame synthetic_bayer_frame(const bool with_matrix = true) {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "synthetic-provider";
    descriptor.provider_version = "synthetic-v1";
    descriptor.storage_dimensions = {4U, 4U};
    descriptor.active_dimensions = descriptor.storage_dimensions;
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.cfa_pattern = "RGGB";
    descriptor.bits_per_sample = 10U;
    descriptor.black_levels = {0U, 0U, 0U, 0U};
    descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
    descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.25};
    if (with_matrix) {
        descriptor.camera_to_linear_srgb_d65 = {
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0,
        };
        descriptor.has_camera_to_linear_srgb_d65 = true;
    }
    frame.samples.resize(16U);
    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = descriptor.bayer_2x2[site];
            const std::uint16_t value = colour == image::RawCfaColor::red
                ? 100U : colour == image::RawCfaColor::green ? 200U : 50U;
            frame.samples[static_cast<std::size_t>(y) * 4U + x] = value;
        }
    }
    return frame;
}

[[nodiscard]] image::PixelBuffer processed_fallback() {
    image::PixelBuffer buffer;
    buffer.dimensions = {4U, 4U};
    buffer.bits_per_channel = 16U;
    buffer.channels = 3U;
    buffer.row_stride_bytes = 4U * 3U * sizeof(std::uint16_t);
    buffer.primaries = image::RgbPrimaries::srgb_rec709_d65;
    buffer.transfer_function = image::RgbTransferFunction::linear;
    buffer.reference = image::RgbBufferReference::processed_raw;
    buffer.samples.assign(4U * 4U * 3U, 7'777U);
    return buffer;
}

class SyntheticRawSession final : public image::DecodeSession {
public:
    explicit SyntheticRawSession(
        image::RawFrame frame,
        std::string normalized_make = {},
        std::string normalized_model = {}
    ) : frame_(std::move(frame)) {
        metadata_.raw_dimensions = frame_.descriptor.active_dimensions;
        metadata_.image_dimensions = frame_.descriptor.active_dimensions;
        metadata_.normalized_make = std::move(normalized_make);
        metadata_.normalized_model = std::move(normalized_model);
        capabilities_.metadata = true;
        capabilities_.raw_frame = true;
        capabilities_.reference_rgb = true;
        capabilities_.raw_development = raw_capabilities();
    }

    [[nodiscard]] const image::AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const image::DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const image::PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] image::PreviewPayload decode_preview(std::size_t) override {
        throw image::DecodeError(image::DecodeErrorCode::no_preview, 0, "no preview");
    }

    [[nodiscard]] image::RawFrame decode_raw_frame() override {
        ++raw_frame_count_;
        return frame_;
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        ++processed_count_;
        return processed_fallback();
    }

    [[nodiscard]] std::size_t raw_frame_count() const noexcept {
        return raw_frame_count_;
    }

    [[nodiscard]] std::size_t processed_count() const noexcept {
        return processed_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    image::RawFrame frame_;
    std::size_t raw_frame_count_ = 0U;
    mutable std::size_t processed_count_ = 0U;
};

} // namespace

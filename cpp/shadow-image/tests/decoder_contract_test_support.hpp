#pragma once

#include <shadow/image/decoder.hpp>
#include <shadow/image/optics.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace shadow::image::test_support {

inline int failures = 0;

inline void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

class FakeRgbSession final : public DecodeSession {
public:
  FakeRgbSession() {
    metadata_.raw_dimensions = {8U, 4U};
    metadata_.image_dimensions = {8U, 4U};
    capabilities_.metadata = true;
    capabilities_.reference_rgb = true;
  }

  [[nodiscard]] const AssetMetadata &metadata() const noexcept override {
    return metadata_;
  }

  [[nodiscard]] const DecodeCapabilities &
  capabilities() const noexcept override {
    return capabilities_;
  }

  [[nodiscard]] std::span<const PreviewDescriptor>
  previews() const noexcept override {
    return {};
  }

  [[nodiscard]] PreviewPayload decode_preview(std::size_t) override {
    throw DecodeError(DecodeErrorCode::no_preview, 0, "no preview");
  }

  [[nodiscard]] RawFrame decode_raw_frame() override {
    throw DecodeError(DecodeErrorCode::unsupported, 0, "no RAW frame");
  }

  [[nodiscard]] PixelBuffer render_reference_rgb() const override {
    ++reference_render_count_;
    PixelBuffer buffer;
    buffer.dimensions = {8, 4};
    buffer.bits_per_channel = 16;
    buffer.channels = 3;
    buffer.row_stride_bytes = 8U * 3U * sizeof(std::uint16_t);
    buffer.primaries = RgbPrimaries::srgb_rec709_d65;
    buffer.transfer_function = RgbTransferFunction::linear;
    buffer.reference = RgbBufferReference::processed_raw;
    buffer.samples.resize(8U * 4U * 3U);
    for (std::size_t index = 0; index < buffer.samples.size(); ++index) {
      buffer.samples[index] =
          static_cast<std::uint16_t>((index * 997U) % 65'536U);
    }
    return buffer;
  }

  [[nodiscard]] std::size_t reference_render_count() const noexcept {
    return reference_render_count_;
  }

private:
  AssetMetadata metadata_;
  DecodeCapabilities capabilities_;
  mutable std::size_t reference_render_count_ = 0;
};

[[nodiscard]] inline PixelBuffer
optics_reference_buffer(const std::uint32_t width = 96U,
                        const std::uint32_t height = 64U) {
  PixelBuffer buffer;
  buffer.dimensions = {width, height};
  buffer.bits_per_channel = 16U;
  buffer.channels = 3U;
  buffer.row_stride_bytes =
      static_cast<std::size_t>(width) * 3U * sizeof(std::uint16_t);
  buffer.primaries = RgbPrimaries::srgb_rec709_d65;
  buffer.transfer_function = RgbTransferFunction::linear;
  buffer.reference = RgbBufferReference::processed_raw;
  buffer.samples.resize(static_cast<std::size_t>(width) * height * 3U);
  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      const auto index = (static_cast<std::size_t>(y) * width + x) * 3U;
      buffer.samples[index] = static_cast<std::uint16_t>(
          static_cast<std::uint64_t>(x) * 65'535U / (width - 1U));
      buffer.samples[index + 1U] = static_cast<std::uint16_t>(
          static_cast<std::uint64_t>(y) * 65'535U / (height - 1U));
      buffer.samples[index + 2U] = static_cast<std::uint16_t>(
          (static_cast<std::uint64_t>(x + y) * 65'535U) /
          (width + height - 2U));
    }
  }
  return buffer;
}

class RetainedRgbSession final : public DecodeSession {
public:
  explicit RetainedRgbSession(PixelBuffer buffer, AssetMetadata metadata = {})
      : metadata_(std::move(metadata)), buffer_(std::move(buffer)) {
    if (metadata_.raw_dimensions.width == 0U ||
        metadata_.raw_dimensions.height == 0U) {
      metadata_.raw_dimensions = buffer_.dimensions;
    }
    if (metadata_.image_dimensions.width == 0U ||
        metadata_.image_dimensions.height == 0U) {
      metadata_.image_dimensions = buffer_.dimensions;
    }
    capabilities_.metadata = true;
    capabilities_.reference_rgb = true;
  }

  [[nodiscard]] const AssetMetadata &metadata() const noexcept override {
    return metadata_;
  }

  [[nodiscard]] const DecodeCapabilities &
  capabilities() const noexcept override {
    return capabilities_;
  }

  [[nodiscard]] std::span<const PreviewDescriptor>
  previews() const noexcept override {
    return {};
  }

  [[nodiscard]] PreviewPayload decode_preview(std::size_t) override {
    throw DecodeError(DecodeErrorCode::no_preview, 0, "no preview");
  }

  [[nodiscard]] RawFrame decode_raw_frame() override {
    throw DecodeError(DecodeErrorCode::unsupported, 0, "no RAW frame");
  }

  [[nodiscard]] PixelBuffer render_reference_rgb() const override {
    return buffer_;
  }

private:
  AssetMetadata metadata_;
  DecodeCapabilities capabilities_;
  PixelBuffer buffer_;
};

class FakeOpticsProvider final : public OpticsProvider {
public:
  [[nodiscard]] const OpticsProviderInfo &info() const noexcept override {
    return info_;
  }

  [[nodiscard]] OpticsCorrectionResult
  correct_reference_rgb(const PixelBuffer &input, const AssetMetadata &,
                        const OpticsSettings &settings) const override {
    ++correction_count_;
    expect(settings.enabled,
           "pipeline sends enabled optics settings to its provider");
    auto corrected = input;
    std::fill(corrected.samples.begin(), corrected.samples.end(), 0U);
    // A third-party optics implementation may allocate or copy only raster
    // pixels. The source-development receipt is owned by preparation and must
    // survive this behaviour.
    corrected.raw_development_receipt = {};
    return {
        .receipt =
            {
                .status = OpticsProfileStatus::matched,
                .provider_id = "fake-optics",
                .provider_version = "test-v1",
                .camera_profile = "Test camera",
                .lens_profile = "Test lens",
                .distortion_available = true,
                .applied_distortion = true,
            },
        .corrected_reference_rgb = std::move(corrected),
    };
  }

  [[nodiscard]] std::size_t correction_count() const noexcept {
    return correction_count_;
  }

private:
  OpticsProviderInfo info_{
      .id = "fake-optics",
      .version = "test-v1",
      .available = true,
  };
  mutable std::size_t correction_count_ = 0U;
};

} // namespace shadow::image::test_support

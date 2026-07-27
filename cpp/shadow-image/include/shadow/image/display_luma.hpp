#pragma once

#include <shadow/image/decoder_types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

/// Final analysis planes never exceed this edge. This is intentionally much
/// smaller than an editing proxy: the consumer computes explainable technical
/// observations, not a display image.
inline constexpr std::uint32_t jpeg_display_luma_max_edge = 512U;

/// Hard limits are checked from the JPEG header before decompression starts.
inline constexpr std::size_t maximum_jpeg_display_luma_encoded_bytes = 128U * 1024U * 1024U;
inline constexpr std::uint32_t maximum_jpeg_display_luma_source_dimension = 65'535U;
inline constexpr std::uint64_t maximum_jpeg_display_luma_source_pixels = 100'000'000U;

/// Every persisted observation must retain this exact preprocessing identity.
///
/// The contract deliberately describes a JPEG display proxy, not RAW sensor
/// data. ICC profiles are not applied, RGB is assumed to be encoded sRGB, EXIF
/// orientation is not interpreted, and Rec.709 coefficients operate in the
/// encoded display domain.
[[nodiscard]] std::string_view jpeg_display_luma_preprocessing_version_prefix() noexcept;

/// An owned, tightly packed plane of normalized display-referred luminance.
/// `row_stride_samples` is measured in `float` samples, never bytes.
struct DisplayLumaImage final {
    Dimensions dimensions;
    std::size_t row_stride_samples = 0U;
    std::vector<float> samples;
    std::string preprocessing_version;
};

/// Decodes a bounded JPEG display proxy into normalized encoded-domain luma.
///
/// The decoder rejects malformed/truncated inputs (including libjpeg warnings),
/// excessive encoded buffers, excessive header dimensions/pixel counts, and
/// intermediate decompression buffers outside the fixed resource envelope.
[[nodiscard]] DisplayLumaImage decode_jpeg_display_luma(
    std::span<const std::uint8_t> encoded,
    std::uint32_t max_edge = jpeg_display_luma_max_edge
);

} // namespace shadow::image

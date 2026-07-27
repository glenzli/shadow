#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/edit.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

// Display output is a distinct, provenance-bearing production stage. It accepts only Shadow's
// standardized linear-sRGB FloatRgbImage contract and produces packed, display-encoded RGB8.
// Warm previews and all source-sized proxy/detail transforms use this dispatcher; resizing keeps
// its separate CPU sampling path until an accelerated resize contract is introduced.
enum class DisplayOutputBackend : std::uint8_t {
    cpu,
    metal,
};

enum class DisplayOutputBackendMode : std::uint8_t {
    automatic,
    cpu,
    metal,
};

inline constexpr std::uint32_t display_output_cpu_backend_version = 1U;
inline constexpr std::uint32_t display_output_metal_backend_version = 1U;

[[nodiscard]] std::string_view display_output_backend_identity(
    DisplayOutputBackend backend
) noexcept;
[[nodiscard]] bool display_output_backend_available(DisplayOutputBackend backend) noexcept;

// Runtime developer/testing override shared with RAW development:
//   SHADOW_IMAGE_ACCELERATION=auto|cpu|metal
// Automatic mode falls back to the CPU oracle when Metal is unavailable or declines a safe
// resource request. Forced Metal fails explicitly and preserves the backend diagnostic.
[[nodiscard]] DisplayOutputBackendMode display_output_backend_mode_from_environment();

struct DisplayOutputRequest final {
    // The first backend intentionally supports only a 1:1 transform. Keeping the target explicit
    // prevents a later resize implementation from silently changing this v1 contract.
    Dimensions target_dimensions;
    // Dither is keyed in image-space coordinates, so independently rendered tiles meet without a
    // visible quantization seam.
    std::uint32_t output_origin_x = 0U;
    std::uint32_t output_origin_y = 0U;
};

struct DisplayRgb8Image final {
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0U;
    std::vector<std::uint8_t> bytes;
    DisplayOutputBackend backend = DisplayOutputBackend::cpu;
    // Automatic mode records why it restarted the complete stage on CPU. Forced CPU/Metal never
    // reports fallback. Diagnostics are runtime-only and must not enter a durable cache identity.
    bool fell_back = false;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept;
};

// Exact CPU oracle for the current JPEG proxy display semantics:
//   scene reference -> neutral scene curve -> Oklab hue-preserving gamut map
//   display reference -> Oklab hue-preserving gamut map
//   both -> sRGB OETF -> coordinate-deterministic luminance dither -> RGB8
[[nodiscard]] DisplayRgb8Image render_linear_srgb_to_display_srgb8_cpu_reference(
    const FloatRgbImage& source,
    DisplayOutputRequest request
);

[[nodiscard]] DisplayRgb8Image render_linear_srgb_to_display_srgb8_with_backend(
    const FloatRgbImage& source,
    DisplayOutputRequest request,
    DisplayOutputBackendMode backend_mode
);

[[nodiscard]] DisplayRgb8Image render_linear_srgb_to_display_srgb8(
    const FloatRgbImage& source,
    DisplayOutputRequest request
);

} // namespace shadow::image

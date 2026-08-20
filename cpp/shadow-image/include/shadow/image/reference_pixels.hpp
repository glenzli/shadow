#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_development_receipt.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shadow::image {

/// Packed, owned provider output at Shadow's processed reference boundary.
struct PixelBuffer final {
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::size_t row_stride_bytes = 0;
    RgbPrimaries primaries = RgbPrimaries::unknown;
    RgbTransferFunction transfer_function = RgbTransferFunction::unknown;
    RgbBufferReference reference = RgbBufferReference::unknown;
    std::vector<std::uint16_t> samples;
    RawDevelopmentReceipt raw_development_receipt;
};

// Version 1 fixes linear gamma, camera WB/matrix conversion, unit brightness, no exposure or
// histogram auto-brightening, and no frame-content adaptive maximum rescaling. Any change to
// those decode semantics must increment this cache-visible contract version.
inline constexpr std::uint32_t processed_linear_reference_rgb_contract_version = 1U;
inline constexpr float processed_linear_reference_maximum_adjustment_threshold = 0.0F;
// The v2 output accepts both standardized scene-referred RAW RGB and standardized display-referred
// raster RGB. Scene-referred RAW uses LibRaw H=0's BT.709-style transfer expressed in linear
// sRGB before the common sRGB OETF: unit scene white remains display white and super-white clips
// instead of receiving a filmic shoulder. JPEG/SDR HEIF keeps its existing display rendering and
// receives only gamut mapping plus the sRGB OETF. JPEG proxy encoding uses 4:4:4 sampling so this
// output contract does not discard chroma detail after rendering. It is a deterministic SDR display
// rendering, not a camera-JPEG emulation.
inline constexpr std::uint32_t display_srgb8_output_transform_version = 2U;
// The gamut mapper is bounded work per out-of-gamut pixel. 0.5 is a conservative ceiling
// above the display-sRGB Oklab gamut; sixteen bisections resolve chroma well below one 8-bit code
// step.
inline constexpr double display_srgb8_maximum_oklab_chroma = 0.5;
inline constexpr std::uint32_t display_srgb8_gamut_search_iterations = 16U;

} // namespace shadow::image

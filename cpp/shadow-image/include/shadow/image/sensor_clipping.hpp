#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cstdint>
#include <vector>

namespace shadow::image {

// A compact, display-oriented diagnostic derived directly from a RawFrame. It is deliberately
// not an edit operation and is never serialized into a Recipe: it says which source samples no
// longer contain recoverable headroom before Shadow applies white balance, demosaic, colour
// transforms, tone mapping, or creative adjustments.
inline constexpr std::uint32_t sensor_clipping_mask_schema_version = 1U;
inline constexpr std::uint32_t sensor_highlight_chroma_confidence_schema_version = 1U;

enum SensorClippingMaskBit : std::uint8_t {
    sensor_highlight_clipped = 1U << 0U,
    sensor_shadow_clipped = 1U << 1U,
};

struct SensorClippingMask final {
    std::uint32_t schema_version = sensor_clipping_mask_schema_version;
    Dimensions dimensions;
    // One bit field per display-oriented output pixel. See SensorClippingMaskBit.
    std::vector<std::uint8_t> samples;
    std::uint64_t highlight_pixel_count = 0U;
    std::uint64_t shadow_pixel_count = 0U;

    [[nodiscard]] bool valid() const noexcept;
};

// One pre-white-balance CFA evidence value per display-oriented output pixel.  `255` means all
// contributing CFA samples retain comfortable physical headroom; smaller values mean that at
// least one measured CFA channel is approaching its calibrated white level.  It is deliberately
// separate from SensorClippingMask: a value can be low before any sample is actually clipped.
// The map is not a Recipe operation and never reconstructs colour; a later highlight edit may
// only use it to avoid treating unreliable chroma as measured detail.
struct SensorHighlightChromaConfidence final {
    std::uint32_t schema_version = sensor_highlight_chroma_confidence_schema_version;
    Dimensions dimensions;
    std::vector<std::uint8_t> samples;

    [[nodiscard]] bool valid() const noexcept;
};

// Projects the active RawFrame into the supplied display dimensions. A target cell is marked as
// highlight-clipped when any source CFA sample reaches its calibrated white level. It is marked
// as shadow-clipped only when every source sample in that cell is at or below its own calibrated
// black level. The latter intentionally avoids labelling ordinary dark image content as lost.
//
// The output is oriented with LibRaw's documented flip convention (0 normal, 3 rotate 180°, 5
// rotate 90° CCW, 6 rotate 90° CW) so it can overlay the processed display proxy directly. This
// function is the materialized CPU reference; the owned RawFrame Metal developer may project the
// same exact target bins beside reconstruction from the original, pre-denoise sensor buffer.
[[nodiscard]] SensorClippingMask
project_sensor_clipping_mask(const RawFrame& frame, Dimensions target_dimensions);

[[nodiscard]] SensorHighlightChromaConfidence
project_sensor_highlight_chroma_confidence(const RawFrame& frame, Dimensions target_dimensions);

} // namespace shadow::image

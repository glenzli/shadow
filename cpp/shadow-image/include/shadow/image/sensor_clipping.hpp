#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shadow::image {

// A compact, display-oriented diagnostic derived directly from a RawFrame. It is deliberately
// not an edit operation and is never serialized into a Recipe: it says which source samples no
// longer contain recoverable headroom before Shadow applies white balance, demosaic, colour
// transforms, tone mapping, or creative adjustments.
inline constexpr std::uint32_t sensor_clipping_mask_schema_version = 3U;

enum SensorClippingMaskBit : std::uint8_t {
    sensor_highlight_clipped = 1U << 0U,
    sensor_shadow_clipped = 1U << 1U,
    // Every CFA colour in the local reconstruction footprint has reached physical white. This is
    // stricter than the factual any-sample highlight flag and gates low-frequency RGB surface
    // reconstruction to a genuinely unknowable terminal core without allocating another plane.
    sensor_shared_highlight_clipped = 1U << 2U,
};

inline constexpr std::uint8_t sensor_clipping_flag_mask = 0x07U;
inline constexpr std::uint8_t sensor_shared_highlight_coverage_shift = 3U;
inline constexpr std::uint8_t sensor_shared_highlight_coverage_levels = 31U;

struct SensorClippingMask final {
    std::uint32_t schema_version = sensor_clipping_mask_schema_version;
    Dimensions dimensions;
    // One packed R8 value per display-oriented output pixel. The low three bits are
    // SensorClippingMaskBit flags; the high five bits store shared-highlight coverage.
    std::vector<std::uint8_t> samples;
    std::uint64_t highlight_pixel_count = 0U;
    std::uint64_t shadow_pixel_count = 0U;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] float shared_highlight_coverage_at(std::size_t pixel) const noexcept;
};

// A display-oriented, source-domain confidence sidecar for highlight chroma. Unlike the binary
// clipping mask it records when CFA chroma is no longer trustworthy before any white balance or
// colour transform: 0 means the measured chroma remains trustworthy and 255 means either
// multi-channel headroom has diverged or every channel shares the terminal response shoulder.
// Spatial luminance reconstruction belongs to RAW source preparation; keeping this sidecar to one
// R8 plane preserves the smallest CPU/Metal Selective Tone hot path.
inline constexpr std::uint32_t highlight_chroma_risk_map_schema_version = 4U;

struct HighlightChromaRiskMap final {
    std::uint32_t schema_version = highlight_chroma_risk_map_schema_version;
    Dimensions dimensions;
    std::vector<std::uint8_t> samples;
    // True when RAW source preparation has consumed the lost-CFA-colour evidence, either inside
    // evidence-owned area integration or through a full-resolution low-frequency colour surface.
    // The physical mask remains factual for diagnostics, while Selective Tone consumes these
    // reconstructed continuous risk samples without overriding them back to a binary
    // physical-white decision. The field name is retained for schema compatibility.
    bool source_surface_reconstructed = false;

    [[nodiscard]] bool valid() const noexcept;
};

// Projects the active RawFrame into the supplied display dimensions. A target cell is marked as
// highlight-clipped when any source CFA sample reaches its calibrated white level. The shared
// highlight bit additionally requires every observed CFA colour in the cell, or a 3x3 demosaic
// footprint at native size, to be physically clipped. The aligned shared-highlight coverage is
// the minimum physically clipped fraction across the three CFA colours, quantised to the exact
// five-bit representation produced by the fused Metal path. A cell is shadow-clipped only when
// every source sample is at or below its own calibrated black level. The latter intentionally
// avoids labelling ordinary dark image content as lost.
//
// The output is oriented with LibRaw's documented flip convention (0 normal, 3 rotate 180°, 5
// rotate 90° CCW, 6 rotate 90° CW) so it can overlay the processed display proxy directly. This
// function is the materialized CPU reference; the owned RawFrame Metal developer may project the
// same exact target bins beside reconstruction from the original, pre-denoise sensor buffer.
[[nodiscard]] SensorClippingMask
project_sensor_clipping_mask(const RawFrame& frame, Dimensions target_dimensions);

// Projects continuous CFA headroom disagreement and a common terminal shoulder into the same
// display grid as `SensorClippingMask`. This is source invariant: changing RAW white balance
// changes the camera rendering, not whether a sensor channel had remaining headroom.
[[nodiscard]] HighlightChromaRiskMap
project_highlight_chroma_risk_map(const RawFrame& frame, Dimensions target_dimensions);

} // namespace shadow::image

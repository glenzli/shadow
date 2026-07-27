#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shadow::image {

// This is deliberately *camera* linear RGB, not the D65 working RGB used by the edit graph.
// Its samples have received only per-CFA black subtraction and white-level normalization from a
// RawFrame. White balance, DNG opcode application, camera calibration, colour management,
// highlight recovery and display rendering remain separate, auditable stages. A provider may
// carry a Camera RGB -> XYZ D50 matrix and an optional per-CFA sensor-noise model on
// RawFrameDescriptor, but this stage deliberately does not apply either white balance, colour
// calibration or noise reduction yet. A later RAW NR stage may consume only a validated numeric
// calibration; it must never inspect a provider's opaque profile database itself.
inline constexpr std::uint32_t raw_demosaic_receipt_schema_version = 1U;

enum class RawDemosaicAlgorithm : std::uint8_t {
    bayer_bilinear_v1,
    // Full-resolution, sensor-domain directional green reconstruction followed by
    // colour-difference interpolation. It deliberately remains separate from the
    // bounded area-preview path: the latter is the responsive interactive tier,
    // while this algorithm spends additional local work only for a requested high
    // quality detail/export render.
    bayer_edge_aware_v1,
    // Preview-only CFA-aware area integration. Each bounded output pixel averages the exact
    // active-sensor footprint independently per colour plane, avoiding full-resolution RGB
    // allocation and the severe moire produced by point-sampling a large Bayer frame.
    bayer_area_preview_v1,
};

struct RawDemosaicReceipt final {
    std::uint32_t schema_version = raw_demosaic_receipt_schema_version;
    std::uint32_t source_raw_frame_schema_version = 0U;
    RawDemosaicAlgorithm algorithm = RawDemosaicAlgorithm::bayer_bilinear_v1;
    bool black_subtraction_applied = false;
    bool white_level_normalization_applied = false;
    bool white_balance_applied = false;
    bool dng_opcodes_applied = false;
};

struct LinearCameraRgbFrame final {
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0U;
    std::vector<float> samples;
    RawDemosaicReceipt receipt;

    [[nodiscard]] bool valid() const noexcept;
};

// The camera transform is allowed to produce values above display white (and, at the edge of a
// camera gamut, small negative components). Keep that scene-linear result in fp32 until the
// common edit/output stages decide how to map it. This is deliberately separate from
// `PixelBuffer`: that type is a packed 16-bit compatibility boundary for decoded/provider RGB
// and cannot represent highlight headroom.
struct SceneLinearRgbFrame final {
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0U;
    std::vector<float> samples;

    [[nodiscard]] bool valid() const noexcept;
};

// First correctness baseline for Bayer sensor development. It samples the full RawFrame in its
// native coordinate system, crops only to the declared active rectangle and reconstructs every
// RGB component by a same-colour 3x3 bilinear neighbourhood. Raw values are normalized as
// `(sample - black[cfa-site]) / (white[cfa-site] - black[cfa-site])` without clipping, so a
// later RAW highlight stage can still decide how to handle super-white samples.
//
// It intentionally rejects X-Trans, Quad Bayer, unknown CFA layouts and malformed frames. The
// result must not be supplied directly to `FloatRgbImage`/the creative graph until a later
// camera-to-working-RGB stage has recorded its calibration and white-balance provenance.
[[nodiscard]] LinearCameraRgbFrame demosaic_bayer_bilinear(const RawFrame& frame);

// Host-owned high-quality Bayer reconstruction. It keeps the same unbounded
// camera-linear contract as the bilinear baseline, but uses directional green
// estimates and local colour differences to reduce zippering and false colour
// at full resolution. Preview callers should keep using demosaic_bayer_preview()
// so fitting an image to the window does not pay a detail/export cost.
[[nodiscard]] LinearCameraRgbFrame demosaic_bayer_edge_aware(const RawFrame& frame);

// Produces an un-oriented camera-linear preview whose longest active-area edge is bounded by
// max_edge. Native-size requests reuse the full bilinear baseline. Downscaled requests integrate
// the complete sensor footprint represented by every output pixel, so total work remains roughly
// proportional to the RAW plane rather than allocating and then resizing a full RGB image.
[[nodiscard]] LinearCameraRgbFrame demosaic_bayer_preview(
    const RawFrame& frame,
    std::uint32_t max_edge
);

} // namespace shadow::image

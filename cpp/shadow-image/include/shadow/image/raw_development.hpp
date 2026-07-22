#pragma once

#include <shadow/image/decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shadow::image {

// This is deliberately *camera* linear RGB, not the D65 working RGB used by the edit graph.
// Its samples have received only per-CFA black subtraction and white-level normalization from a
// RawFrame. White balance, DNG opcode application, camera calibration, colour management,
// highlight recovery and display rendering remain separate, auditable stages. A provider may
// carry a Camera RGB -> XYZ D50 matrix on RawFrameDescriptor, but this stage deliberately does
// not apply it yet.
inline constexpr std::uint32_t raw_demosaic_receipt_schema_version = 1U;

enum class RawDemosaicAlgorithm : std::uint8_t {
    bayer_bilinear_v1,
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

} // namespace shadow::image

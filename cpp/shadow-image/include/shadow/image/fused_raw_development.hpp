#pragma once

#include <shadow/image/raw_development.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace shadow::image {

// Camera RGB -> linear sRGB/Rec.709 D65, row-major. The caller compiles all color decisions into
// this one immutable matrix before rendering:
//
// - the generic provider route folds AsShotNeutral white balance into its camera matrix;
// - the DCP route passes DcpColorTransform::camera_to_linear_srgb_d65, which already contains
//   white balance, chromatic adaptation, and BaselineExposureOffset.
//
// Keeping profile interpretation outside this hot loop lets the same bounded renderer serve
// public LibRaw and independently implemented provider paths without learning either profile
// format.
struct RawFrameLinearTransform final {
    std::array<double, 9U> camera_to_linear_srgb_d65{};

    [[nodiscard]] bool valid() const noexcept;
};

// The fused renderer returns the same public PixelBuffer boundary and demosaic provenance as the
// reference two-stage path, but never allocates a full-resolution float Camera-RGB image.
struct FusedRawFrameDevelopment final {
    PixelBuffer pixels;
    RawDemosaicReceipt demosaic_receipt;

    [[nodiscard]] bool valid() const noexcept;
};

// Reconstructs Bayer samples, applies the precompiled camera transform, maps the provider's
// orientation, clamps only at the current u16 scene-linear boundary, and writes the final
// PixelBuffer in one bounded parallel row pass.
//
// A missing preview edge selects full-resolution 3x3 bilinear reconstruction. A non-zero preview
// edge selects the same CFA-aware sensor-footprint integration as demosaic_bayer_preview().
// Existing standalone demosaic functions remain the correctness/reference API.
[[nodiscard]] FusedRawFrameDevelopment develop_bayer_linear_srgb_u16_fused(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge = std::nullopt
);

} // namespace shadow::image

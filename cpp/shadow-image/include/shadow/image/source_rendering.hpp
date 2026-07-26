#pragma once

#include <shadow/image/decoder.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/source_profile_catalog.hpp>

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace shadow::image {

struct FloatRgbImage;

// Source rendering is the small, non-destructive bridge between a decoder's
// standardized linear RGB and Shadow's editable scene-referred image. It is
// intentionally not an AdjustmentNode: a photographer's Recipe must remain
// portable while a public camera/profile package may evolve independently.
//
// Version 1 supports Shadow Standard plus public exposure/luminance-curve
// source profiles in already-standardized linear RGB. DCP matrices and
// camera-space LUTs remain deliberately outside this boundary until RawFrame
// exists; a private local provider may still report its own opaque identity
// without placing vendor assets in Shadow.
inline constexpr std::uint32_t source_rendering_schema_version = 1U;
inline constexpr std::uint32_t source_rendering_implementation_version = 1U;

enum class SourceRenderingKind : std::uint8_t {
    // Decoded JPEG/HEIF already has a display rendering. Keep its appearance
    // and let only the common edit graph and output transform handle it.
    embedded_rendering,
    // A processed RAW starts from LibRaw's deliberately neutral linear RGB.
    // Shadow Standard applies DNG calibration when present, otherwise a
    // bounded scene exposure normalization that avoids the chronically dark
    // appearance of a bare matrix-only render.
    shadow_standard,
    // Exact match from a declarative source-profile package. Version 1 only
    // carries public exposure calibration; a future profile payload can grow
    // here without changing how private decoder data is kept out of Shadow.
    public_profile,
};

struct SourceRenderingReceipt final {
    std::uint32_t schema_version = source_rendering_schema_version;
    std::string profile_id;
    std::string profile_identity;
    SourceRenderingKind kind = SourceRenderingKind::embedded_rendering;
    // `camera_baseline_exposure_stops` is a valid DNG BaselineExposure. It is
    // source metadata, never an editable exposure slider value.
    double camera_baseline_exposure_stops = 0.0;
    // Shadow Standard's public, image-content-bounded lift. This remains zero
    // for DNG because an explicit DNG baseline is a better calibration than a
    // generic percentile estimate, and it remains zero for rendered raster.
    double standard_exposure_normalization_stops = 0.0;
    double profile_exposure_stops = 0.0;
    std::vector<SourceToneCurvePoint> luminance_tone_curve;

    [[nodiscard]] double total_exposure_stops() const noexcept {
        return camera_baseline_exposure_stops + standard_exposure_normalization_stops
            + profile_exposure_stops;
    }

    auto operator<=>(const SourceRenderingReceipt&) const = default;
};

// Resolves the public source renderer from standardized source pixels and
// metadata. The result includes every data-dependent scalar needed to apply
// the same source appearance to warm previews and full-resolution tiles.
[[nodiscard]] SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata
);

[[nodiscard]] SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata,
    const SourceProfileCatalog& catalog
);

// RawFrame callers must pass the typed pipeline receipt. An applied DCP suppresses the
// camera-specific public/maker source profile (its color calibration already happened), while
// preserving Shadow Standard's neutral scene mapping and the source file's distinct DNG
// BaselineExposure. A no-match or fail-closed local profile keeps the ordinary generic path.
[[nodiscard]] SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata,
    const RawPipelineReceipt& pipeline
);

[[nodiscard]] SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata,
    const RawPipelineReceipt& pipeline,
    const SourceProfileCatalog& catalog
);

// Owned RawFrame development retains this fp32 source rather than first quantizing it to the
// provider-RGB compatibility buffer. The resulting receipt is identical in meaning to the
// PixelBuffer overload, but percentile normalization observes real super-white samples.
[[nodiscard]] SourceRenderingReceipt resolve_source_rendering(
    const SceneLinearRgbFrame& source,
    const AssetMetadata& metadata,
    const RawPipelineReceipt& pipeline
);

// Applies a previously resolved receipt to a linear working image. Calling it
// with a receipt resolved for a different source-rendering schema is an error;
// silently interpreting future profile data would poison preview caches.
void apply_source_rendering(
    FloatRgbImage& image,
    const SourceRenderingReceipt& receipt
);

// A stable cache/provenance string. The caller normally stores the receipt,
// but this helper keeps every future cache-key construction canonical.
[[nodiscard]] std::string source_rendering_identity(const SourceRenderingReceipt& receipt);

} // namespace shadow::image

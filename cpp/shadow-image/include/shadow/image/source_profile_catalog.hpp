#pragma once

#include <shadow/image/decoder.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace shadow::image {

// A `shadow-source-profile-v1` document is intentionally declarative. It can
// carry an exact camera match (or an explicit maker fallback), an exposure
// calibration, and one monotone luminance curve for Shadow's already-
// standardized linear RGB. It cannot embed a vendor SDK blob, a raw decoder
// setting, camera-space LUT, or reverse-engineered camera data. Publicly
// redistributable packages and private local adapters can therefore use one
// safe boundary.
inline constexpr std::uint32_t source_profile_document_schema_version = 1U;

struct SourceToneCurvePoint final {
    // Normalized linear-light luminance domain and normalized output. Source
    // profiles are deliberately limited to monotone one-dimensional curves;
    // camera-space LUTs belong to the later RawFrame/DCP pipeline.
    double input = 0.0;
    double output = 0.0;

    auto operator<=>(const SourceToneCurvePoint&) const = default;
};

struct SourceProfileDefinition final {
    std::string id;
    std::string display_name;
    std::string normalized_make;
    std::string normalized_model;
    // This is a source-rendering calibration, not a user Exposure node.
    double display_exposure_stops = 0.0;
    // True only when `display_exposure_stops` is a camera/profile calibration
    // that should replace Shadow Standard's content-bounded fallback. A
    // generic maker look provides no exposure calibration and must not make a
    // genuinely dark RAW remain dark merely because it supplies a curve.
    bool has_exposure_calibration = true;
    // Empty means that this profile changes only the source exposure. A
    // non-empty curve is evaluated from luminance and its gain is applied to
    // all RGB channels, keeping neutral colors neutral and avoiding a
    // per-channel base-curve color shift.
    std::vector<SourceToneCurvePoint> luminance_tone_curve;
    // Content fingerprint, never the file path. Modifying a profile changes
    // the renderer/cache identity even when its filename stays unchanged.
    std::string content_identity;
};

struct SourceProfileCatalog final {
    std::vector<SourceProfileDefinition> profiles;
    std::string identity;
};

// Reads `*.shadow-source-profile` documents in one explicit directory. Invalid
// or incomplete documents are ignored for now; later UI can surface catalog
// diagnostics without making a malformed optional profile block photo import.
[[nodiscard]] SourceProfileCatalog load_source_profile_catalog(
    const std::filesystem::path& directory
);

// GPL-compatible camera looks bundled with Shadow. They are intentionally a
// small display-referred source-rendering package, not a claim of DCP-level
// camera color matching. Local exact-camera profiles take precedence over
// these manufacturer fallbacks.
[[nodiscard]] SourceProfileCatalog load_builtin_source_profile_catalog();

// Uses SHADOW_SOURCE_PROFILE_DIRECTORY when set. Otherwise it searches the
// app-private platform profile directory. This directory is deliberately not
// a plugin/binary directory and is safe to scan in the desktop process.
[[nodiscard]] SourceProfileCatalog load_local_source_profile_catalog();

[[nodiscard]] std::optional<SourceProfileDefinition> match_source_profile(
    const SourceProfileCatalog& catalog,
    const AssetMetadata& metadata
);

} // namespace shadow::image

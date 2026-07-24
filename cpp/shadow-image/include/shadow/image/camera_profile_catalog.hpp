#pragma once

#include <shadow/image/camera_profile.hpp>
#include <shadow/image/decoder.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shadow::image {

inline constexpr std::uint32_t camera_profile_catalog_schema_version = 1U;

enum class CameraProfileDiagnosticCode : std::uint8_t {
    directory_unavailable,
    directory_iteration_failed,
    file_read_failed,
    profile_parse_failed,
    duplicate_camera_model,
};

struct CameraProfileDiagnostic final {
    CameraProfileDiagnosticCode code = CameraProfileDiagnosticCode::profile_parse_failed;
    // Only the final filename is retained. Optional local profile paths must not leak into
    // exported recipes, cache identities, bug reports, or private-provider boundaries.
    std::string source_name;
    std::string message;
};

struct CameraProfileDefinition final {
    DcpProfile profile;
    // Canonical exact-match key derived from DCP UniqueCameraModel.
    std::string normalized_camera_model;
    // SHA-256 of the complete DCP bytes. The path and mtime are deliberately excluded so
    // replacing a file in place invalidates every dependent source/cache representation.
    std::string content_identity;
    std::string source_name;
};

struct CameraProfileCatalog final {
    std::uint32_t schema_version = camera_profile_catalog_schema_version;
    std::vector<CameraProfileDefinition> profiles;
    std::vector<CameraProfileDiagnostic> diagnostics;
    std::string identity;
};

// ASCII camera identifiers in DCP/EXIF are case- and whitespace-normalized, but are not fuzzily
// rewritten. Exact matching is important: "EOS R", "EOS R5", and "EOS R5 Mark II" must never
// share a calibration merely because their names look related.
[[nodiscard]] std::string normalize_camera_profile_model(std::string_view value);
[[nodiscard]] std::string normalized_camera_profile_model(const AssetMetadata& metadata);

// Loads only regular `.dcp` files immediately inside one explicit directory. Invalid optional
// profiles become diagnostics and never prevent an otherwise supported RAW from opening.
[[nodiscard]] CameraProfileCatalog load_camera_profile_catalog(
    const std::filesystem::path& directory
);

// Uses SHADOW_CAMERA_PROFILE_DIRECTORY when configured. Otherwise it searches the per-user
// application data directory (`camera-profiles/dcp`). Shadow ships no camera profile through
// this function; every returned profile is user-local and remains subject to its own policy.
[[nodiscard]] CameraProfileCatalog load_local_camera_profile_catalog();

// Process-lifetime catalog for the render pipeline. A future profile-manager UI can replace this
// with an explicitly refreshable snapshot; today the immutable snapshot keeps parallel preview
// jobs deterministic and avoids scanning/parsing the directory for every image.
[[nodiscard]] const CameraProfileCatalog& default_camera_profile_catalog();

// The returned pointer is owned by the immutable catalog snapshot. Avoid copying a complete DCP
// (which may contain large HueSat/Look tables) for every preview or import worker.
[[nodiscard]] const CameraProfileDefinition* match_camera_profile(
    const CameraProfileCatalog& catalog,
    const AssetMetadata& metadata
);

} // namespace shadow::image

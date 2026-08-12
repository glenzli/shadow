#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace shadow::image {

/// Photographer-facing scope for one deterministic geometry analysis.
enum class AutoGeometryMode : std::uint8_t {
    automatic = 0U,
    level = 1U,
    vertical = 2U,
    full = 3U,
};

/// Borrowed display-sRGB RGB8 preview used only for bounded geometry analysis.
/// The caller retains the bytes for the complete call. The analyzer downsamples
/// internally and never retains or mutates the raster.
struct AutoGeometryRgb8View final {
    std::span<const std::uint8_t> pixels;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::size_t row_stride_bytes = 0U;
};

/// One non-authoritative correction proposal against identity fine geometry.
/// Values use the existing PhotoGeometry units. Applying or persisting them is
/// deliberately owned by the caller; analysis never edits a photo.
struct AutoGeometryProposal final {
    AutoGeometryMode mode = AutoGeometryMode::automatic;
    bool available = false;
    double straighten_degrees = 0.0;
    double perspective_vertical = 0.0;
    double perspective_horizontal = 0.0;
    double confidence = 0.0;
    std::uint32_t supporting_lines = 0U;
    std::uint32_t vertical_lines = 0U;
    std::uint32_t horizontal_lines = 0U;
};

/// Detects dominant architectural/horizon geometry without a model runtime.
/// Invalid byte layouts fail closed with std::invalid_argument. Valid but
/// inconclusive photographs return an unavailable proposal.
[[nodiscard]] AutoGeometryProposal analyze_auto_geometry(
    AutoGeometryRgb8View source,
    AutoGeometryMode mode = AutoGeometryMode::automatic
);

} // namespace shadow::image

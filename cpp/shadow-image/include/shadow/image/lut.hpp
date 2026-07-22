#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

/// Validated three-dimensional IRIDAS/Adobe `.cube` lookup table.
///
/// Entries use the file format's canonical order: red varies fastest, then
/// green, then blue. Domain values are scene/display coordinates, not an
/// implied color space; the caller owns the pipeline placement.
struct CubeLut3D final {
    std::string title;
    std::uint16_t size = 0;
    std::array<double, 3> domain_min{0.0, 0.0, 0.0};
    std::array<double, 3> domain_max{1.0, 1.0, 1.0};
    std::vector<std::array<float, 3>> entries;

    auto operator<=>(const CubeLut3D&) const = default;
};

/// Parses a bounded 3D `.cube` document and rejects ambiguous or malformed
/// files. The first implementation deliberately rejects 1D LUTs instead of
/// silently treating them as 3D color transforms.
[[nodiscard]] CubeLut3D parse_cube_lut(std::string_view document);

/// Samples a validated LUT with clamped-domain trilinear interpolation.
[[nodiscard]] std::array<float, 3> sample_cube_lut(
    const CubeLut3D& lut,
    std::array<float, 3> input
);

} // namespace shadow::image

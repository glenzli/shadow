#pragma once

#include <shadow/image/decoder_types.hpp>

#include <compare>
#include <array>
#include <cstdint>

namespace shadow::image {

struct FloatRgbImage;

/// Lossless right-angle orientation applied to the final photo canvas.
///
/// This is intentionally outside the adjustment-node enum: crop and
/// orientation alter output dimensions, while a node transforms samples in an
/// already-established raster. The source-coordinate Grade Node graph and
/// photo-local repair pass therefore execute before this state is applied.
enum class PhotoQuarterTurn : std::uint8_t {
    zero = 0U,
    clockwise_90 = 1U,
    clockwise_180 = 2U,
    clockwise_270 = 3U,
};

struct PhotoGeometry final {
    double crop_left = 0.0;
    double crop_top = 0.0;
    double crop_right = 1.0;
    double crop_bottom = 1.0;
    PhotoQuarterTurn quarter_turn = PhotoQuarterTurn::zero;
    // Fine rotation automatically narrows the final canvas to remove the
    // empty corners it would otherwise create, while retaining this crop's
    // aspect ratio.
    double straighten_degrees = 0.0;
    // Symmetric keystone correction in oriented-photo coordinates. Values
    // are normalized to [-1, 1]; zero is the exact no-perspective identity.
    double perspective_vertical = 0.0;
    double perspective_horizontal = 0.0;
    bool flip_horizontal = false;
    bool flip_vertical = false;

    auto operator<=>(const PhotoGeometry&) const = default;
};

struct GeometryPixelRect final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    auto operator<=>(const GeometryPixelRect&) const = default;
};

/// A validated integer crop and its final output dimensions. This one layout
/// is shared by complete warm-proxy execution and bounded full-detail tiles,
/// so their crop edges can never diverge due to independent rounding rules.
struct PhotoGeometryLayout final {
    GeometryPixelRect source_crop;
    Dimensions output_dimensions;

    auto operator<=>(const PhotoGeometryLayout&) const = default;
};

void validate_photo_geometry(const PhotoGeometry& geometry);
[[nodiscard]] PhotoGeometryLayout photo_geometry_layout(
    Dimensions source_dimensions,
    const PhotoGeometry& geometry
);
/// Maps continuous normalized output-edge coordinates through the same integer
/// crop and half-pixel convention as preview, detail and export sampling.
[[nodiscard]] std::array<double, 2> photo_geometry_source_point(
    Dimensions source_dimensions, const PhotoGeometry& geometry, double x, double y
);
[[nodiscard]] GeometryPixelRect photo_geometry_source_rect_for_output(
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    GeometryPixelRect output_rect
);
[[nodiscard]] FloatRgbImage apply_photo_geometry(
    const FloatRgbImage& source,
    const PhotoGeometry& geometry
);
[[nodiscard]] FloatRgbImage apply_photo_geometry_tile(
    const FloatRgbImage& source_tile,
    GeometryPixelRect source_tile_rect,
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    GeometryPixelRect output_rect
);

} // namespace shadow::image

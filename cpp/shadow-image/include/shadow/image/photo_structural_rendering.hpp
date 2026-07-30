#pragma once

#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>

#include <optional>

namespace shadow::image {

/// One resolution-specific execution plan for the fixed structural order
/// `Liquify -> Canvas`.
///
/// Canvas remains mandatory. Liquify is optional and prepared exactly once;
/// source-ROI queries and RGB execution consume this same immutable plan.
struct PreparedPhotoStructuralRendering final {
    Dimensions source_dimensions;
    PhotoGeometry geometry;
    PhotoGeometryLayout geometry_layout;
    std::optional<PreparedPhotoLiquify> liquify;
};

[[nodiscard]] PreparedPhotoStructuralRendering prepare_photo_structural_rendering(
    Dimensions source_dimensions,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify = nullptr
);

/// Returns a conservative original-image preimage for one Canvas output
/// rectangle. When Liquify is present, the geometry footprint is expanded by
/// the prepared plan's maximum displacement bound.
[[nodiscard]] GeometryPixelRect photo_structural_source_rect_for_output(
    const PreparedPhotoStructuralRendering& structural,
    GeometryPixelRect output_rect
);

/// View form used by native backends that already retain the authoritative
/// Canvas layout and an optional prepared Liquify plan.
[[nodiscard]] GeometryPixelRect photo_structural_source_rect_for_output(
    const PhotoGeometryLayout& geometry_layout,
    const PhotoGeometry& geometry,
    const PreparedPhotoLiquify* liquify,
    GeometryPixelRect output_rect
);

/// Executes optional Liquify and mandatory Canvas through one bilinear RGB
/// sample per output pixel.
[[nodiscard]] FloatRgbImage apply_photo_structural_rendering(
    const FloatRgbImage& source,
    const PreparedPhotoStructuralRendering& structural
);

/// Tile-local form of the same single-sample contract.
[[nodiscard]] FloatRgbImage apply_photo_structural_rendering_tile(
    const FloatRgbImage& source_tile,
    GeometryPixelRect source_tile_rect,
    const PreparedPhotoStructuralRendering& structural,
    GeometryPixelRect output_rect
);

} // namespace shadow::image

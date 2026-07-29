#pragma once

#include <shadow/image/photo_geometry.hpp>

#include <cmath>
#include <cstdint>

namespace shadow::image::detail {

struct PhotoGeometrySourceCoordinate final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] inline bool
photo_geometry_is_transposed(const PhotoQuarterTurn quarter_turn) noexcept {
    return quarter_turn == PhotoQuarterTurn::clockwise_90
        || quarter_turn == PhotoQuarterTurn::clockwise_270;
}

[[nodiscard]] inline Dimensions photo_geometry_oriented_crop_dimensions(
    const GeometryPixelRect crop,
    const PhotoGeometry& geometry
) noexcept {
    return Dimensions{
        .width =
            photo_geometry_is_transposed(geometry.quarter_turn)
            ? crop.height
            : crop.width,
        .height =
            photo_geometry_is_transposed(geometry.quarter_turn)
            ? crop.width
            : crop.height,
    };
}

// Canonical inverse mapping shared by RGB geometry and transient scalar mask coverage. Keeping
// the half-pixel convention here prevents selection overlays from drifting from their paired
// preview when crop, orientation, flip, or straighten changes.
[[nodiscard]] inline PhotoGeometrySourceCoordinate
photo_geometry_source_coordinate_for_output(
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    const std::uint32_t output_x,
    const std::uint32_t output_y
) {
    const std::uint32_t crop_width = layout.source_crop.width;
    const std::uint32_t crop_height = layout.source_crop.height;
    const double output_width =
        static_cast<double>(layout.output_dimensions.width);
    const double output_height =
        static_cast<double>(layout.output_dimensions.height);
    const Dimensions full_oriented =
        photo_geometry_oriented_crop_dimensions(layout.source_crop, geometry);
    const double full_oriented_width =
        static_cast<double>(full_oriented.width);
    const double full_oriented_height =
        static_cast<double>(full_oriented.height);
    const double angle = geometry.straighten_degrees
        * 3.141592653589793238462643383279502884 / 180.0;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double output_dx =
        static_cast<double>(output_x) + 0.5 - output_width * 0.5;
    const double output_dy =
        static_cast<double>(output_y) + 0.5 - output_height * 0.5;
    const double oriented_x =
        std::fma(cosine, output_dx, sine * output_dy)
        + full_oriented_width * 0.5;
    const double oriented_y =
        std::fma(-sine, output_dx, cosine * output_dy)
        + full_oriented_height * 0.5;

    double crop_x = 0.0;
    double crop_y = 0.0;
    switch (geometry.quarter_turn) {
    case PhotoQuarterTurn::zero:
        crop_x = oriented_x;
        crop_y = oriented_y;
        break;
    case PhotoQuarterTurn::clockwise_90:
        crop_x = oriented_y;
        crop_y = static_cast<double>(crop_height) - oriented_x;
        break;
    case PhotoQuarterTurn::clockwise_180:
        crop_x = static_cast<double>(crop_width) - oriented_x;
        crop_y = static_cast<double>(crop_height) - oriented_y;
        break;
    case PhotoQuarterTurn::clockwise_270:
        crop_x = static_cast<double>(crop_width) - oriented_y;
        crop_y = oriented_x;
        break;
    }
    if (geometry.flip_horizontal) {
        crop_x = static_cast<double>(crop_width) - crop_x;
    }
    if (geometry.flip_vertical) {
        crop_y = static_cast<double>(crop_height) - crop_y;
    }
    return PhotoGeometrySourceCoordinate{
        .x = static_cast<double>(layout.source_crop.x) + crop_x - 0.5,
        .y = static_cast<double>(layout.source_crop.y) + crop_y - 0.5,
    };
}

} // namespace shadow::image::detail

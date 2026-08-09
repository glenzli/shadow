#pragma once

#include <shadow/image/photo_geometry.hpp>

#include <cmath>
#include <cstdint>

namespace shadow::image::detail {

struct PhotoGeometrySourceCoordinate final {
    double x = 0.0;
    double y = 0.0;
};

struct PhotoGeometryNormalizedCoordinate final {
    double x = 0.0;
    double y = 0.0;
};

// Maps the complete output square into an interior symmetric trapezoid. The
// formula is an exact homography, so straight lines remain straight and the
// corner-only detail-tile footprint remains authoritative. The weaker edge
// retains at least half its extent at the maximum authored correction.
[[nodiscard]] inline PhotoGeometryNormalizedCoordinate
photo_geometry_apply_perspective(
    PhotoGeometryNormalizedCoordinate coordinate,
    const double vertical,
    const double horizontal
) noexcept {
    if (vertical == 0.0 && horizontal == 0.0) {
        return coordinate;
    }
    const auto edge_scales = [](const double amount) {
        const double reduced = 1.0 - 0.5 * std::abs(amount);
        return amount >= 0.0
            ? PhotoGeometryNormalizedCoordinate{reduced, 1.0}
            : PhotoGeometryNormalizedCoordinate{1.0, reduced};
    };
    const auto vertical_scales = edge_scales(vertical);
    const double vertical_sum = vertical_scales.x + vertical_scales.y;
    const double vertical_c =
        (vertical_scales.x - vertical_scales.y) / vertical_sum;
    const double vertical_k =
        2.0 * vertical_scales.x * vertical_scales.y / vertical_sum;
    const double vertical_denominator = 1.0 + vertical_c * coordinate.y;
    coordinate = {
        .x = vertical_k * coordinate.x / vertical_denominator,
        .y = (coordinate.y + vertical_c) / vertical_denominator,
    };

    const auto horizontal_scales = edge_scales(horizontal);
    const double horizontal_sum = horizontal_scales.x + horizontal_scales.y;
    const double horizontal_c =
        (horizontal_scales.x - horizontal_scales.y) / horizontal_sum;
    const double horizontal_k =
        2.0 * horizontal_scales.x * horizontal_scales.y / horizontal_sum;
    const double horizontal_denominator = 1.0 + horizontal_c * coordinate.x;
    return {
        .x = (coordinate.x + horizontal_c) / horizontal_denominator,
        .y = horizontal_k * coordinate.y / horizontal_denominator,
    };
}

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
    double oriented_x =
        std::fma(cosine, output_dx, sine * output_dy)
        + full_oriented_width * 0.5;
    double oriented_y =
        std::fma(-sine, output_dx, cosine * output_dy)
        + full_oriented_height * 0.5;
    if (geometry.perspective_vertical != 0.0 || geometry.perspective_horizontal != 0.0) {
        const PhotoGeometryNormalizedCoordinate perspective = photo_geometry_apply_perspective(
            {
                .x = oriented_x / full_oriented_width * 2.0 - 1.0,
                .y = oriented_y / full_oriented_height * 2.0 - 1.0,
            },
            geometry.perspective_vertical,
            geometry.perspective_horizontal
        );
        oriented_x = (perspective.x + 1.0) * 0.5 * full_oriented_width;
        oriented_y = (perspective.y + 1.0) * 0.5 * full_oriented_height;
    }

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

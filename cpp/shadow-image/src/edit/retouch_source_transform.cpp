#include "retouch_source_transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace shadow::image::detail {

namespace {

constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;

[[nodiscard]] std::array<std::array<double, 2U>, 4U>
corners(const RetouchRasterBounds bounds) noexcept {
    return {
        {{{bounds.lower_x, bounds.lower_y}},
         {{bounds.upper_x, bounds.lower_y}},
         {{bounds.lower_x, bounds.upper_y}},
         {{bounds.upper_x, bounds.upper_y}}}
    };
}

} // namespace

RetouchSourceMapping
RetouchSourceMapping::with_local_origin(const double origin_x, const double origin_y) const {
    RetouchSourceMapping result = *this;
    result.anchor_x -= origin_x;
    result.anchor_y -= origin_y;
    return result;
}

double RetouchSourceMapping::source_x(const double target_x, const double target_y) const noexcept {
    const double delta_x = target_x - anchor_x;
    const double delta_y = target_y - anchor_y;
    return anchor_x + offset_x + std::fma(matrix_xx, delta_x, matrix_xy * delta_y);
}

double RetouchSourceMapping::source_y(const double target_x, const double target_y) const noexcept {
    const double delta_x = target_x - anchor_x;
    const double delta_y = target_y - anchor_y;
    return anchor_y + offset_y + std::fma(matrix_yx, delta_x, matrix_yy * delta_y);
}

RetouchSourceMapping make_retouch_source_mapping(
    const double rotation_degrees,
    const double scale,
    const bool flip_horizontal,
    const bool flip_vertical,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y,
    const double anchor_x,
    const double anchor_y,
    const double offset_x,
    const double offset_y
) {
    const double radians = rotation_degrees * degrees_to_radians;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double flip_x = flip_horizontal ? -1.0 : 1.0;
    const double flip_y = flip_vertical ? -1.0 : 1.0;
    return {
        .matrix_xx = scale * cosine * flip_x,
        .matrix_xy =
            -scale * sine * flip_y * level_zero_to_raster_scale_x / level_zero_to_raster_scale_y,
        .matrix_yx =
            scale * sine * flip_x * level_zero_to_raster_scale_y / level_zero_to_raster_scale_x,
        .matrix_yy = scale * cosine * flip_y,
        .anchor_x = anchor_x,
        .anchor_y = anchor_y,
        .offset_x = offset_x,
        .offset_y = offset_y,
    };
}

std::optional<RetouchSourceMapping> clamp_retouch_source_mapping_to_image(
    RetouchSourceMapping mapping,
    const RetouchRasterBounds target_bounds,
    const Dimensions full_dimensions
) {
    if (full_dimensions.width == 0U || full_dimensions.height == 0U) {
        return std::nullopt;
    }
    double lower_x = std::numeric_limits<double>::infinity();
    double upper_x = -std::numeric_limits<double>::infinity();
    double lower_y = std::numeric_limits<double>::infinity();
    double upper_y = -std::numeric_limits<double>::infinity();
    for (const auto corner : corners(target_bounds)) {
        const double x = mapping.source_x(corner[0], corner[1]);
        const double y = mapping.source_y(corner[0], corner[1]);
        lower_x = std::min(lower_x, x);
        upper_x = std::max(upper_x, x);
        lower_y = std::min(lower_y, y);
        upper_y = std::max(upper_y, y);
    }
    const double maximum_x = static_cast<double>(full_dimensions.width - 1U);
    const double maximum_y = static_cast<double>(full_dimensions.height - 1U);
    if (!std::isfinite(lower_x) || !std::isfinite(upper_x) || !std::isfinite(lower_y)
        || !std::isfinite(upper_y) || upper_x - lower_x > maximum_x
        || upper_y - lower_y > maximum_y) {
        return std::nullopt;
    }
    if (lower_x < 0.0) {
        mapping.offset_x -= lower_x;
        upper_x -= lower_x;
    }
    if (upper_x > maximum_x) {
        mapping.offset_x -= upper_x - maximum_x;
    }
    if (lower_y < 0.0) {
        mapping.offset_y -= lower_y;
        upper_y -= lower_y;
    }
    if (upper_y > maximum_y) {
        mapping.offset_y -= upper_y - maximum_y;
    }
    return mapping;
}

RetouchRasterBounds retouch_source_displacement_bounds(
    const RetouchSourceMapping& mapping,
    const RetouchRasterBounds target_bounds
) noexcept {
    RetouchRasterBounds result{
        .lower_x = std::numeric_limits<double>::infinity(),
        .upper_x = -std::numeric_limits<double>::infinity(),
        .lower_y = std::numeric_limits<double>::infinity(),
        .upper_y = -std::numeric_limits<double>::infinity(),
    };
    for (const auto corner : corners(target_bounds)) {
        const double displacement_x = mapping.source_x(corner[0], corner[1]) - corner[0];
        const double displacement_y = mapping.source_y(corner[0], corner[1]) - corner[1];
        result.lower_x = std::min(result.lower_x, displacement_x);
        result.upper_x = std::max(result.upper_x, displacement_x);
        result.lower_y = std::min(result.lower_y, displacement_y);
        result.upper_y = std::max(result.upper_y, displacement_y);
    }
    return result;
}

RetouchSourceReach retouch_source_reach(
    const SpotHealAdjustment& adjustment,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y,
    const Dimensions raster_dimensions
) {
    RetouchSourceReach result;
    const auto include = [&](const std::uint16_t radius_level_zero_pixels,
                             const double authored_offset_x_radii,
                             const double authored_offset_y_radii,
                             const double rotation_degrees,
                             const double source_scale,
                             const bool flip_horizontal,
                             const bool flip_vertical,
                             const double normalized_lower_x,
                             const double normalized_upper_x,
                             const double normalized_lower_y,
                             const double normalized_upper_y,
                             const bool horizontal_stroke) {
        const double radius_x =
            static_cast<double>(radius_level_zero_pixels) * level_zero_to_raster_scale_x;
        const double radius_y =
            static_cast<double>(radius_level_zero_pixels) * level_zero_to_raster_scale_y;
        const bool automatic = authored_offset_x_radii == 0.0 && authored_offset_y_radii == 0.0;
        const double center_x = std::midpoint(normalized_lower_x, normalized_upper_x);
        const double center_y = std::midpoint(normalized_lower_y, normalized_upper_y);
        const double automatic_distance =
            std::min(3.0, maximum_retouch_source_offset_radii(radius_level_zero_pixels));
        const double offset_x =
            automatic
                ? (horizontal_stroke
                       ? 0.0
                       : (center_x <= 0.5 ? automatic_distance : -automatic_distance) * radius_x)
                : authored_offset_x_radii * radius_x;
        const double offset_y =
            automatic
                ? (horizontal_stroke
                       ? (center_y <= 0.5 ? automatic_distance : -automatic_distance) * radius_y
                       : 0.0)
                : authored_offset_y_radii * radius_y;
        const bool identity =
            rotation_degrees == 0.0 && source_scale == 1.0 && !flip_horizontal && !flip_vertical;
        if ((raster_dimensions.width == 0U || raster_dimensions.height == 0U) && !identity) {
            result.horizontal =
                maximum_retouch_detail_apron_level_zero_pixels * level_zero_to_raster_scale_x;
            result.vertical =
                maximum_retouch_detail_apron_level_zero_pixels * level_zero_to_raster_scale_y;
            return;
        }
        if (raster_dimensions.width == 0U || raster_dimensions.height == 0U) {
            const double reach_x = automatic ? automatic_distance * radius_x : std::abs(offset_x);
            const double reach_y = automatic ? automatic_distance * radius_y : std::abs(offset_y);
            result.horizontal = std::max(result.horizontal, reach_x + radius_x + 1.0);
            result.vertical = std::max(result.vertical, reach_y + radius_y + 1.0);
            return;
        }
        const double anchor_x = center_x * static_cast<double>(raster_dimensions.width) - 0.5;
        const double anchor_y = center_y * static_cast<double>(raster_dimensions.height) - 0.5;
        const RetouchRasterBounds bounds{
            .lower_x =
                normalized_lower_x * static_cast<double>(raster_dimensions.width) - 0.5 - radius_x,
            .upper_x =
                normalized_upper_x * static_cast<double>(raster_dimensions.width) - 0.5 + radius_x,
            .lower_y =
                normalized_lower_y * static_cast<double>(raster_dimensions.height) - 0.5 - radius_y,
            .upper_y =
                normalized_upper_y * static_cast<double>(raster_dimensions.height) - 0.5 + radius_y,
        };
        const auto mapping = clamp_retouch_source_mapping_to_image(
            make_retouch_source_mapping(
                rotation_degrees,
                source_scale,
                flip_horizontal,
                flip_vertical,
                level_zero_to_raster_scale_x,
                level_zero_to_raster_scale_y,
                anchor_x,
                anchor_y,
                offset_x,
                offset_y
            ),
            bounds,
            raster_dimensions
        );
        if (!mapping.has_value()) {
            result.horizontal =
                maximum_retouch_detail_apron_level_zero_pixels * level_zero_to_raster_scale_x;
            result.vertical =
                maximum_retouch_detail_apron_level_zero_pixels * level_zero_to_raster_scale_y;
            return;
        }
        const RetouchRasterBounds displacement =
            retouch_source_displacement_bounds(*mapping, bounds);
        result.horizontal = std::max(
            result.horizontal,
            std::max(std::abs(displacement.lower_x), std::abs(displacement.upper_x)) + radius_x
                + 1.0
        );
        result.vertical = std::max(
            result.vertical,
            std::max(std::abs(displacement.lower_y), std::abs(displacement.upper_y)) + radius_y
                + 1.0
        );
    };

    for (const SpotHealTarget& target : adjustment.spots) {
        include(
            target.radius_level_zero_pixels,
            target.source_offset_x_radii,
            target.source_offset_y_radii,
            target.source_rotation_degrees,
            target.source_scale,
            target.source_flip_horizontal,
            target.source_flip_vertical,
            target.center_x,
            target.center_x,
            target.center_y,
            target.center_y,
            false
        );
    }
    for (const RetouchStroke& stroke : adjustment.strokes) {
        double lower_x = 1.0;
        double upper_x = 0.0;
        double lower_y = 1.0;
        double upper_y = 0.0;
        for (const RetouchStrokePoint point : stroke.points) {
            lower_x = std::min(lower_x, point.x);
            upper_x = std::max(upper_x, point.x);
            lower_y = std::min(lower_y, point.y);
            upper_y = std::max(upper_y, point.y);
        }
        include(
            stroke.radius_level_zero_pixels,
            stroke.source_offset_x_radii,
            stroke.source_offset_y_radii,
            stroke.source_rotation_degrees,
            stroke.source_scale,
            stroke.source_flip_horizontal,
            stroke.source_flip_vertical,
            lower_x,
            upper_x,
            lower_y,
            upper_y,
            upper_x - lower_x >= upper_y - lower_y
        );
    }
    return result;
}

} // namespace shadow::image::detail

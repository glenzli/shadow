#include "edit_liquify_coordinates.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

std::optional<QPointF> EditLiquifyCoordinates::originalPointForOutput(
    const QPointF output,
    const double output_aspect_ratio,
    const BackendPhotoGeometry& geometry
) {
    if (!std::isfinite(output_aspect_ratio) || output_aspect_ratio <= 0.0) {
        return std::nullopt;
    }
    const double angle = geometry.straighten_degrees * std::numbers::pi_v<double> / 180.0;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double absolute_cosine = std::abs(cosine);
    const double absolute_sine = std::abs(sine);
    const double oriented_height_over_width = 1.0 / output_aspect_ratio;
    const double retained_scale = std::min(
        1.0 / (absolute_cosine + absolute_sine * oriented_height_over_width),
        1.0 / (absolute_sine * output_aspect_ratio + absolute_cosine)
    );
    const double output_dx = output.x() - 0.5;
    const double output_dy = output.y() - 0.5;
    const double oriented_x =
        0.5 + retained_scale * (cosine * output_dx + sine * output_dy * oriented_height_over_width);
    const double oriented_y =
        0.5 + retained_scale * (-sine * output_dx * output_aspect_ratio + cosine * output_dy);

    double crop_x = oriented_x;
    double crop_y = oriented_y;
    switch (geometry.quarter_turn) {
    case 0:
        break;
    case 1:
        crop_x = oriented_y;
        crop_y = 1.0 - oriented_x;
        break;
    case 2:
        crop_x = 1.0 - oriented_x;
        crop_y = 1.0 - oriented_y;
        break;
    case 3:
        crop_x = 1.0 - oriented_y;
        crop_y = oriented_x;
        break;
    default:
        return std::nullopt;
    }
    if (geometry.flip_horizontal) {
        crop_x = 1.0 - crop_x;
    }
    if (geometry.flip_vertical) {
        crop_y = 1.0 - crop_y;
    }
    const double original_x =
        geometry.crop_left + crop_x * (geometry.crop_right - geometry.crop_left);
    const double original_y =
        geometry.crop_top + crop_y * (geometry.crop_bottom - geometry.crop_top);
    if (!std::isfinite(original_x) || !std::isfinite(original_y)) {
        return std::nullopt;
    }
    return QPointF{
        std::clamp(original_x, 0.0, 1.0),
        std::clamp(original_y, 0.0, 1.0),
    };
}

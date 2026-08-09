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
    double oriented_x =
        0.5 + retained_scale * (cosine * output_dx + sine * output_dy * oriented_height_over_width);
    double oriented_y =
        0.5 + retained_scale * (-sine * output_dx * output_aspect_ratio + cosine * output_dy);

    const auto edge_scales = [](const double amount) {
        const double reduced = 1.0 - 0.5 * std::abs(amount);
        return amount >= 0.0 ? QPointF{reduced, 1.0} : QPointF{1.0, reduced};
    };
    if (geometry.perspective_vertical != 0.0 || geometry.perspective_horizontal != 0.0) {
        double perspective_x = oriented_x * 2.0 - 1.0;
        double perspective_y = oriented_y * 2.0 - 1.0;
    const QPointF vertical_scales = edge_scales(geometry.perspective_vertical);
    const double vertical_sum = vertical_scales.x() + vertical_scales.y();
    const double vertical_c = (vertical_scales.x() - vertical_scales.y()) / vertical_sum;
    const double vertical_k =
        2.0 * vertical_scales.x() * vertical_scales.y() / vertical_sum;
    const double vertical_denominator = 1.0 + vertical_c * perspective_y;
    perspective_x = vertical_k * perspective_x / vertical_denominator;
    perspective_y = (perspective_y + vertical_c) / vertical_denominator;
    const QPointF horizontal_scales = edge_scales(geometry.perspective_horizontal);
    const double horizontal_sum = horizontal_scales.x() + horizontal_scales.y();
    const double horizontal_c =
        (horizontal_scales.x() - horizontal_scales.y()) / horizontal_sum;
    const double horizontal_k =
        2.0 * horizontal_scales.x() * horizontal_scales.y() / horizontal_sum;
    const double horizontal_denominator = 1.0 + horizontal_c * perspective_x;
    perspective_y = horizontal_k * perspective_y / horizontal_denominator;
    perspective_x = (perspective_x + horizontal_c) / horizontal_denominator;
        oriented_x = (perspective_x + 1.0) * 0.5;
        oriented_y = (perspective_y + 1.0) * 0.5;
    }

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

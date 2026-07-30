#pragma once

#include <shadow/image/photo_liquify.hpp>

#include <algorithm>
#include <cmath>

namespace shadow::image::detail {

struct PhotoLiquifySourceCoordinate final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] inline double photo_liquify_smootherstep(const double value) noexcept {
    const double bounded = std::clamp(value, 0.0, 1.0);
    return bounded * bounded * bounded
        * (bounded * (bounded * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] inline double photo_liquify_stamp_weight(
    const PreparedPhotoLiquifyStamp& stamp,
    const double x,
    const double y
) noexcept {
    const double distance = std::hypot(x - stamp.center_x, y - stamp.center_y);
    if (distance >= stamp.radius) {
        return 0.0;
    }
    const double normalized_distance = distance / stamp.radius;
    if (stamp.hardness >= 1.0 || normalized_distance <= stamp.hardness) {
        return 1.0;
    }
    const double falloff_position =
        (normalized_distance - stamp.hardness) / (1.0 - stamp.hardness);
    return 1.0 - photo_liquify_smootherstep(falloff_position);
}

/// Applies the prepared displacement in reverse authoring order without
/// touching RGB samples. Planning and every final sampler consume this exact
/// coordinate contract.
[[nodiscard]] inline PhotoLiquifySourceCoordinate
inverse_photo_liquify_coordinate(
    const PreparedPhotoLiquify& liquify,
    double x,
    double y
) noexcept {
    for (auto stamp = liquify.stamps.rbegin(); stamp != liquify.stamps.rend(); ++stamp) {
        const double weight = photo_liquify_stamp_weight(*stamp, x, y);
        x -= stamp->displacement_x * weight;
        y -= stamp->displacement_y * weight;
    }
    return PhotoLiquifySourceCoordinate{.x = x, .y = y};
}

} // namespace shadow::image::detail

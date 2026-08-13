#pragma once

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/decoder_types.hpp>

#include <optional>

namespace shadow::image::detail {

struct RetouchRasterBounds final {
    double lower_x = 0.0;
    double upper_x = 0.0;
    double lower_y = 0.0;
    double upper_y = 0.0;
};

struct RetouchSourceReach final {
    double horizontal = 0.0;
    double vertical = 0.0;
};

// Affine donor-coordinate mapping in the currently rendered raster. The
// target anchor and translation remain explicit so full-image clamping can be
// performed before converting the anchor into a detail tile's local origin.
struct RetouchSourceMapping final {
    double matrix_xx = 1.0;
    double matrix_xy = 0.0;
    double matrix_yx = 0.0;
    double matrix_yy = 1.0;
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;

    [[nodiscard]] RetouchSourceMapping with_local_origin(double origin_x, double origin_y) const;
    [[nodiscard]] double source_x(double target_x, double target_y) const noexcept;
    [[nodiscard]] double source_y(double target_x, double target_y) const noexcept;
};

[[nodiscard]] RetouchSourceMapping make_retouch_source_mapping(
    double rotation_degrees,
    double scale,
    bool flip_horizontal,
    bool flip_vertical,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y,
    double anchor_x,
    double anchor_y,
    double offset_x,
    double offset_y
);

// Keeps the transformed donor footprint inside the complete image by moving
// only its translation. A transform whose source footprint is larger than the
// image cannot be represented without implicit edge replication and is
// rejected.
[[nodiscard]] std::optional<RetouchSourceMapping> clamp_retouch_source_mapping_to_image(
    RetouchSourceMapping mapping,
    RetouchRasterBounds target_bounds,
    Dimensions full_dimensions
);

// Conservative target-to-donor reach over an axis-aligned target footprint.
// Affine extrema occur at a corner, making this exact for the supplied bounds.
[[nodiscard]] RetouchRasterBounds retouch_source_displacement_bounds(
    const RetouchSourceMapping& mapping,
    RetouchRasterBounds target_bounds
) noexcept;

[[nodiscard]] RetouchSourceReach retouch_source_reach(
    const SpotHealAdjustment& adjustment,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y,
    Dimensions raster_dimensions = {}
);

} // namespace shadow::image::detail

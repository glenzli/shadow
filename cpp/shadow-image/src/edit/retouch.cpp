#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/retouch.hpp>
#include <shadow/image/working_rgb.hpp>

#include "retouch_frequency.hpp"
#include "retouch_heal_blending.hpp"
#include "retouch_source_transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr std::size_t maximum_spot_heal_targets = 64U;
constexpr std::size_t maximum_retouch_strokes = 64U;
constexpr std::size_t maximum_retouch_stroke_points = 512U;
constexpr std::uint16_t minimum_spot_radius_level_zero = 1U;
constexpr std::uint16_t maximum_spot_radius_level_zero = 128U;

[[nodiscard]] bool source_offset_fits_detail_apron(
    const std::uint16_t radius,
    const double horizontal,
    const double vertical,
    const double source_scale
) noexcept {
    const double maximum = maximum_retouch_source_offset_radii(radius) + 1.0 - source_scale;
    return std::abs(horizontal) <= maximum && std::abs(vertical) <= maximum;
}

[[noreturn]] void invalid_retouch(const std::string_view detail) {
    throw EditError(
        EditErrorCode::invalid_parameter,
        std::nullopt,
        "spot-heal adjustment " + std::string(detail)
    );
}

[[nodiscard]] Dimensions
execution_full_dimensions(const FloatRgbImage& image, const AdjustmentExecutionContext context) {
    const Dimensions full =
        context.full_dimensions.width == 0U || context.full_dimensions.height == 0U
            ? image.dimensions
            : context.full_dimensions;
    if (full.width == 0U || full.height == 0U || context.origin_x > full.width
        || context.origin_y > full.height || image.dimensions.width > full.width - context.origin_x
        || image.dimensions.height > full.height - context.origin_y) {
        invalid_retouch("execution context exceeds full-image dimensions");
    }
    return full;
}

[[nodiscard]] double smoothstep(const double edge0, const double edge1, const double value) {
    if (edge1 <= edge0) {
        return value < edge1 ? 0.0 : 1.0;
    }
    const double normalized = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

[[nodiscard]] std::size_t
sample_index(const FloatRgbImage& image, const std::uint32_t x, const std::uint32_t y) {
    return static_cast<std::size_t>(y) * (image.row_stride_bytes / sizeof(float))
           + static_cast<std::size_t>(x) * rgb_channels;
}

[[nodiscard]] std::array<double, rgb_channels>
sample_bilinear(const FloatRgbImage& image, const double x, const double y) {
    const double clamped_x = std::clamp(x, 0.0, static_cast<double>(image.dimensions.width - 1U));
    const double clamped_y = std::clamp(y, 0.0, static_cast<double>(image.dimensions.height - 1U));
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const auto x1 = std::min(x0 + 1U, image.dimensions.width - 1U);
    const auto y1 = std::min(y0 + 1U, image.dimensions.height - 1U);
    const double blend_x = clamped_x - static_cast<double>(x0);
    const double blend_y = clamped_y - static_cast<double>(y0);
    const std::size_t top_left = sample_index(image, x0, y0);
    const std::size_t top_right = sample_index(image, x1, y0);
    const std::size_t bottom_left = sample_index(image, x0, y1);
    const std::size_t bottom_right = sample_index(image, x1, y1);

    std::array<double, rgb_channels> result{};
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        const double top = std::lerp(
            static_cast<double>(image.samples[top_left + channel]),
            static_cast<double>(image.samples[top_right + channel]),
            blend_x
        );
        const double bottom = std::lerp(
            static_cast<double>(image.samples[bottom_left + channel]),
            static_cast<double>(image.samples[bottom_right + channel]),
            blend_x
        );
        result[channel] = std::lerp(top, bottom, blend_y);
    }
    return result;
}

struct RasterPoint final {
    double x = 0.0;
    double y = 0.0;
};

struct RasterBounds final {
    std::int64_t lower_x = 0;
    std::int64_t upper_x = -1;
    std::int64_t lower_y = 0;
    std::int64_t upper_y = -1;
};

[[nodiscard]] RasterPoint raster_point(
    const RetouchStrokePoint point,
    const Dimensions full,
    const AdjustmentExecutionContext context
) {
    return RasterPoint{
        .x =
            point.x * static_cast<double>(full.width) - static_cast<double>(context.origin_x) - 0.5,
        .y = point.y * static_cast<double>(full.height) - static_cast<double>(context.origin_y)
             - 0.5,
    };
}

[[nodiscard]] RasterBounds capsule_bounds(
    const RasterPoint first,
    const RasterPoint last,
    const double radius_x,
    const double radius_y,
    const double radius_multiplier
) {
    return RasterBounds{
        .lower_x = static_cast<std::int64_t>(
            std::floor(std::min(first.x, last.x) - radius_x * radius_multiplier)
        ),
        .upper_x = static_cast<std::int64_t>(
            std::ceil(std::max(first.x, last.x) + radius_x * radius_multiplier)
        ),
        .lower_y = static_cast<std::int64_t>(
            std::floor(std::min(first.y, last.y) - radius_y * radius_multiplier)
        ),
        .upper_y = static_cast<std::int64_t>(
            std::ceil(std::max(first.y, last.y) + radius_y * radius_multiplier)
        ),
    };
}

// A brush has a circular user-facing radius even on non-square pixels. Measure
// the capsule in that normalized ellipse space, so the swept coverage joins
// without the visible individual-dab seams of the old spot approximation.
[[nodiscard]] double capsule_distance(
    const double x,
    const double y,
    const RasterPoint first,
    const RasterPoint last,
    const double radius_x,
    const double radius_y
) {
    const double point_x = (x - first.x) / radius_x;
    const double point_y = (y - first.y) / radius_y;
    const double segment_x = (last.x - first.x) / radius_x;
    const double segment_y = (last.y - first.y) / radius_y;
    const double segment_length_squared = std::fma(segment_x, segment_x, segment_y * segment_y);
    const double projection =
        segment_length_squared <= std::numeric_limits<double>::epsilon()
            ? 0.0
            : std::clamp(
                  (point_x * segment_x + point_y * segment_y) / segment_length_squared,
                  0.0,
                  1.0
              );
    const double delta_x = point_x - projection * segment_x;
    const double delta_y = point_y - projection * segment_y;
    return std::sqrt(std::fma(delta_x, delta_x, delta_y * delta_y));
}

[[nodiscard]] double brush_alpha(const double distance, const double feather) {
    // Subtracting a tile origin must not move a mathematically exact circle
    // boundary outside a hard brush because of double rounding.
    if (distance > 1.0 + 1.0e-12) {
        return 0.0;
    }
    return feather <= 0.0 ? 1.0 : 1.0 - smoothstep(1.0 - feather, 1.0, distance);
}

[[nodiscard]] bool valid_retouch_properties(
    const std::uint16_t radius_level_zero_pixels,
    const SpotRepairMode mode,
    const double source_offset_x_radii,
    const double source_offset_y_radii,
    const double source_rotation_degrees,
    const double source_scale,
    const double feather,
    const double strength
) {
    return radius_level_zero_pixels >= minimum_spot_radius_level_zero
           && radius_level_zero_pixels <= maximum_spot_radius_level_zero
           && (mode == SpotRepairMode::heal || mode == SpotRepairMode::clone
               || mode == SpotRepairMode::heal_structure || mode == SpotRepairMode::tone
               || mode == SpotRepairMode::texture)
           && std::isfinite(source_offset_x_radii) && std::isfinite(source_offset_y_radii)
           && std::isfinite(source_rotation_degrees) && source_rotation_degrees >= -180.0
           && source_rotation_degrees <= 180.0 && std::isfinite(source_scale)
           && source_scale >= 0.25 && source_scale <= 4.0
           && source_offset_fits_detail_apron(
               radius_level_zero_pixels,
               source_offset_x_radii,
               source_offset_y_radii,
               source_scale
           )
           && std::isfinite(feather) && feather >= 0.0 && feather <= 1.0 && std::isfinite(strength)
           && strength >= 0.0 && strength <= 1.0;
}

struct SourceOffsetPixels final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] SourceOffsetPixels source_offset_pixels(
    const double authored_x_radii,
    const double authored_y_radii,
    const double radius_x,
    const double radius_y,
    const std::uint16_t radius_level_zero_pixels,
    const double normalized_center_x,
    const double normalized_center_y
) {
    constexpr double authored_zero_epsilon = 1.0e-9;
    if (std::abs(authored_x_radii) > authored_zero_epsilon
        || std::abs(authored_y_radii) > authored_zero_epsilon) {
        return {
            .x = authored_x_radii * radius_x,
            .y = authored_y_radii * radius_y,
        };
    }
    // A zero authored offset is the durable "automatic source" identity used
    // by legacy and newly created Heal regions. Resolve it deterministically
    // for rendering and expose the same effective source through the editor.
    const double automatic_distance =
        std::min(3.0, maximum_retouch_source_offset_radii(radius_level_zero_pixels));
    return {
        .x = (normalized_center_x <= 0.5 ? automatic_distance : -automatic_distance) * radius_x,
        .y = (normalized_center_y <= 0.5 ? automatic_distance * 0.5 : -automatic_distance * 0.5)
             * radius_y,
    };
}

void apply_target(
    FloatRgbImage& image,
    const SpotHealTarget target,
    const AdjustmentExecutionContext context,
    const Dimensions full
) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U) {
        invalid_retouch("requires a non-empty raster");
    }
    if (target.strength <= 0.0) {
        return;
    }
    const double radius_x =
        static_cast<double>(target.radius_level_zero_pixels) * image.level_zero_to_raster_scale_x;
    const double radius_y =
        static_cast<double>(target.radius_level_zero_pixels) * image.level_zero_to_raster_scale_y;
    if (!std::isfinite(radius_x) || !std::isfinite(radius_y) || radius_x <= 0.0
        || radius_y <= 0.0) {
        invalid_retouch("has an invalid proxy scale");
    }

    // Pixel-centre coordinates map exactly to the normalized original image
    // contract used by local masks. The input tile may contain a detail apron.
    const double center_x = target.center_x * static_cast<double>(full.width)
                            - static_cast<double>(context.origin_x) - 0.5;
    const double center_y = target.center_y * static_cast<double>(full.height)
                            - static_cast<double>(context.origin_y) - 0.5;
    const auto paint_lower_x = static_cast<std::int64_t>(std::floor(center_x - radius_x)) - 1;
    const auto paint_upper_x = static_cast<std::int64_t>(std::ceil(center_x + radius_x)) + 1;
    const auto paint_lower_y = static_cast<std::int64_t>(std::floor(center_y - radius_y)) - 1;
    const auto paint_upper_y = static_cast<std::int64_t>(std::ceil(center_y + radius_y)) + 1;
    const std::int64_t raster_max_x = static_cast<std::int64_t>(image.dimensions.width) - 1;
    const std::int64_t raster_max_y = static_cast<std::int64_t>(image.dimensions.height) - 1;
    const std::int64_t lower_x = std::max<std::int64_t>(0, paint_lower_x);
    const std::int64_t upper_x = std::min(raster_max_x, paint_upper_x);
    const std::int64_t lower_y = std::max<std::int64_t>(0, paint_lower_y);
    const std::int64_t upper_y = std::min(raster_max_y, paint_upper_y);
    if (lower_x > upper_x || lower_y > upper_y) {
        return;
    }
    // Tiles outside this region need neither a source snapshot nor a frequency split.
    const FloatRgbImage source = image;
    const auto coverage_width = static_cast<std::uint32_t>(upper_x - lower_x + 1);
    const auto coverage_height = static_cast<std::uint32_t>(upper_y - lower_y + 1);
    std::vector<float> coverage(static_cast<std::size_t>(coverage_width) * coverage_height, 0.0F);
    for (std::int64_t y = lower_y; y <= upper_y; ++y) {
        for (std::int64_t x = lower_x; x <= upper_x; ++x) {
            const double dx = (static_cast<double>(x) - center_x) / radius_x;
            const double dy = (static_cast<double>(y) - center_y) / radius_y;
            const double distance = std::sqrt(std::fma(dx, dx, dy * dy));
            const double alpha = brush_alpha(distance, target.feather);
            coverage
                [static_cast<std::size_t>(y - lower_y) * coverage_width
                 + static_cast<std::size_t>(x - lower_x)] = static_cast<float>(alpha);
        }
    }
    const SourceOffsetPixels donor_offset = source_offset_pixels(
        target.source_offset_x_radii,
        target.source_offset_y_radii,
        radius_x,
        radius_y,
        target.radius_level_zero_pixels,
        target.center_x,
        target.center_y
    );
    const double full_center_x = target.center_x * static_cast<double>(full.width) - 0.5;
    const double full_center_y = target.center_y * static_cast<double>(full.height) - 0.5;
    const auto full_mapping = detail::clamp_retouch_source_mapping_to_image(
        detail::make_retouch_source_mapping(
            target.source_rotation_degrees,
            target.source_scale,
            target.source_flip_horizontal,
            target.source_flip_vertical,
            image.level_zero_to_raster_scale_x,
            image.level_zero_to_raster_scale_y,
            full_center_x,
            full_center_y,
            donor_offset.x,
            donor_offset.y
        ),
        detail::RetouchRasterBounds{
            .lower_x = full_center_x - radius_x,
            .upper_x = full_center_x + radius_x,
            .lower_y = full_center_y - radius_y,
            .upper_y = full_center_y + radius_y,
        },
        full
    );
    if (!full_mapping.has_value()) {
        invalid_retouch("source transform exceeds the full image");
    }
    const detail::RetouchSourceMapping source_mapping =
        full_mapping->with_local_origin(context.origin_x, context.origin_y);
    if (target.mode == SpotRepairMode::tone || target.mode == SpotRepairMode::texture) {
        detail::apply_frequency_retouch(
            image,
            source,
            coverage,
            lower_x,
            lower_y,
            coverage_width,
            coverage_height,
            source_mapping,
            target.strength,
            target.mode == SpotRepairMode::texture,
            target.frequency_radius
        );
        return;
    }
    if (target.mode != SpotRepairMode::clone) {
        detail::apply_texture_heal(
            image,
            source,
            coverage,
            lower_x,
            lower_y,
            coverage_width,
            coverage_height,
            source_mapping,
            target.strength,
            target.mode == SpotRepairMode::heal_structure
        );
        return;
    }
    for (std::int64_t y = lower_y; y <= upper_y; ++y) {
        for (std::int64_t x = lower_x; x <= upper_x; ++x) {
            const double alpha = target.strength
                                 * coverage
                                     [static_cast<std::size_t>(y - lower_y) * coverage_width
                                      + static_cast<std::size_t>(x - lower_x)];
            if (alpha <= 0.0) {
                continue;
            }
            const std::size_t sample =
                sample_index(image, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const std::array<double, rgb_channels> replacement = sample_bilinear(
                source,
                source_mapping.source_x(static_cast<double>(x), static_cast<double>(y)),
                source_mapping.source_y(static_cast<double>(x), static_cast<double>(y))
            );
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const double value = std::fma(
                    alpha,
                    replacement[channel] - static_cast<double>(source.samples[sample + channel]),
                    static_cast<double>(source.samples[sample + channel])
                );
                if (!std::isfinite(value)
                    || value < static_cast<double>(std::numeric_limits<float>::lowest())
                    || value > static_cast<double>(std::numeric_limits<float>::max())) {
                    invalid_retouch("produced an invalid RGB sample");
                }
                image.samples[sample + channel] = static_cast<float>(value);
            }
        }
    }
}

void apply_stroke(
    FloatRgbImage& image,
    const RetouchStroke& stroke,
    const AdjustmentExecutionContext context,
    const Dimensions full
) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U) {
        invalid_retouch("requires a non-empty raster");
    }
    if (stroke.strength <= 0.0) {
        return;
    }
    const double radius_x =
        static_cast<double>(stroke.radius_level_zero_pixels) * image.level_zero_to_raster_scale_x;
    const double radius_y =
        static_cast<double>(stroke.radius_level_zero_pixels) * image.level_zero_to_raster_scale_y;
    if (!std::isfinite(radius_x) || !std::isfinite(radius_y) || radius_x <= 0.0
        || radius_y <= 0.0) {
        invalid_retouch("has an invalid proxy scale");
    }
    if (image.dimensions.width
        > std::numeric_limits<std::size_t>::max() / image.dimensions.height) {
        invalid_retouch("stroke coverage exceeds the supported raster range");
    }

    std::vector<RasterPoint> points;
    points.reserve(stroke.points.size());
    for (const auto point : stroke.points) {
        points.push_back(raster_point(point, full, context));
    }

    const std::int64_t raster_max_x = static_cast<std::int64_t>(image.dimensions.width) - 1;
    const std::int64_t raster_max_y = static_cast<std::int64_t>(image.dimensions.height) - 1;
    std::int64_t stroke_lower_x = raster_max_x;
    std::int64_t stroke_upper_x = 0;
    std::int64_t stroke_lower_y = raster_max_y;
    std::int64_t stroke_upper_y = 0;

    // A one-point gesture is a valid single circular dab. For two or more
    // points, each adjacent pair contributes one round-ended capsule.
    const std::size_t segment_count = points.size() == 1U ? 1U : points.size() - 1U;
    for (std::size_t segment = 0U; segment < segment_count; ++segment) {
        const RasterPoint first = points[segment];
        const RasterPoint last = points[points.size() == 1U ? 0U : segment + 1U];
        const RasterBounds bounds = capsule_bounds(first, last, radius_x, radius_y, 1.0);
        const std::int64_t lower_x = std::max<std::int64_t>(0, bounds.lower_x);
        const std::int64_t upper_x = std::min(raster_max_x, bounds.upper_x);
        const std::int64_t lower_y = std::max<std::int64_t>(0, bounds.lower_y);
        const std::int64_t upper_y = std::min(raster_max_y, bounds.upper_y);
        if (lower_x > upper_x || lower_y > upper_y) {
            continue;
        }
        stroke_lower_x = std::min(stroke_lower_x, lower_x);
        stroke_upper_x = std::max(stroke_upper_x, upper_x);
        stroke_lower_y = std::min(stroke_lower_y, lower_y);
        stroke_upper_y = std::max(stroke_upper_y, upper_y);
    }
    if (stroke_lower_x > stroke_upper_x || stroke_lower_y > stroke_upper_y) {
        return;
    }
    // Tiles outside this region need neither a source snapshot nor a frequency split.
    const FloatRgbImage source = image;
    stroke_lower_x = std::max<std::int64_t>(0, stroke_lower_x - 1);
    stroke_upper_x = std::min(raster_max_x, stroke_upper_x + 1);
    stroke_lower_y = std::max<std::int64_t>(0, stroke_lower_y - 1);
    stroke_upper_y = std::min(raster_max_y, stroke_upper_y + 1);
    const auto coverage_width = static_cast<std::uint32_t>(stroke_upper_x - stroke_lower_x + 1);
    const auto coverage_height = static_cast<std::uint32_t>(stroke_upper_y - stroke_lower_y + 1);
    std::vector<float> coverage(static_cast<std::size_t>(coverage_width) * coverage_height, 0.0F);
    // Maximum segment coverage produces one continuous union with no dab
    // seams, while the compact allocation keeps a long thin stroke from
    // paying for the complete image on every interactive render.
    for (std::size_t segment = 0U; segment < segment_count; ++segment) {
        const RasterPoint first = points[segment];
        const RasterPoint last = points[points.size() == 1U ? 0U : segment + 1U];
        const RasterBounds bounds = capsule_bounds(first, last, radius_x, radius_y, 1.0);
        const std::int64_t lower_x = std::max<std::int64_t>(0, bounds.lower_x);
        const std::int64_t upper_x = std::min(raster_max_x, bounds.upper_x);
        const std::int64_t lower_y = std::max<std::int64_t>(0, bounds.lower_y);
        const std::int64_t upper_y = std::min(raster_max_y, bounds.upper_y);
        for (std::int64_t y = lower_y; y <= upper_y; ++y) {
            for (std::int64_t x = lower_x; x <= upper_x; ++x) {
                const double alpha = brush_alpha(
                    capsule_distance(
                        static_cast<double>(x),
                        static_cast<double>(y),
                        first,
                        last,
                        radius_x,
                        radius_y
                    ),
                    stroke.feather
                );
                const std::size_t coverage_index =
                    static_cast<std::size_t>(y - stroke_lower_y) * coverage_width
                    + static_cast<std::size_t>(x - stroke_lower_x);
                coverage[coverage_index] =
                    std::max(coverage[coverage_index], static_cast<float>(alpha));
            }
        }
    }

    double normalized_lower_x = 1.0;
    double normalized_upper_x = 0.0;
    double normalized_lower_y = 1.0;
    double normalized_upper_y = 0.0;
    for (const auto point : stroke.points) {
        normalized_lower_x = std::min(normalized_lower_x, point.x);
        normalized_upper_x = std::max(normalized_upper_x, point.x);
        normalized_lower_y = std::min(normalized_lower_y, point.y);
        normalized_upper_y = std::max(normalized_upper_y, point.y);
    }
    const double normalized_center_x = std::midpoint(normalized_lower_x, normalized_upper_x);
    const double normalized_center_y = std::midpoint(normalized_lower_y, normalized_upper_y);
    const bool automatic_source =
        stroke.source_offset_x_radii == 0.0 && stroke.source_offset_y_radii == 0.0;
    const double automatic_distance =
        std::min(3.0, maximum_retouch_source_offset_radii(stroke.radius_level_zero_pixels));
    const SourceOffsetPixels authored_donor_offset =
        automatic_source
        ? (normalized_upper_x - normalized_lower_x >= normalized_upper_y - normalized_lower_y
            ? SourceOffsetPixels{
                .x = 0.0,
                .y =
                    (normalized_center_y <= 0.5 ? automatic_distance : -automatic_distance)
                    * radius_y,
            }
            : SourceOffsetPixels{
                .x =
                    (normalized_center_x <= 0.5 ? automatic_distance : -automatic_distance)
                    * radius_x,
                .y = 0.0,
            })
        : source_offset_pixels(
            stroke.source_offset_x_radii,
            stroke.source_offset_y_radii,
            radius_x,
            radius_y,
            stroke.radius_level_zero_pixels,
            normalized_center_x,
            normalized_center_y
        );
    const double full_anchor_x = normalized_center_x * static_cast<double>(full.width) - 0.5;
    const double full_anchor_y = normalized_center_y * static_cast<double>(full.height) - 0.5;
    const auto full_mapping = detail::clamp_retouch_source_mapping_to_image(
        detail::make_retouch_source_mapping(
            stroke.source_rotation_degrees,
            stroke.source_scale,
            stroke.source_flip_horizontal,
            stroke.source_flip_vertical,
            image.level_zero_to_raster_scale_x,
            image.level_zero_to_raster_scale_y,
            full_anchor_x,
            full_anchor_y,
            authored_donor_offset.x,
            authored_donor_offset.y
        ),
        detail::RetouchRasterBounds{
            .lower_x = normalized_lower_x * static_cast<double>(full.width) - 0.5 - radius_x,
            .upper_x = normalized_upper_x * static_cast<double>(full.width) - 0.5 + radius_x,
            .lower_y = normalized_lower_y * static_cast<double>(full.height) - 0.5 - radius_y,
            .upper_y = normalized_upper_y * static_cast<double>(full.height) - 0.5 + radius_y,
        },
        full
    );
    if (!full_mapping.has_value()) {
        invalid_retouch("continuous stroke source transform exceeds the full image");
    }
    const detail::RetouchSourceMapping source_mapping =
        full_mapping->with_local_origin(context.origin_x, context.origin_y);
    if (stroke.mode == SpotRepairMode::tone || stroke.mode == SpotRepairMode::texture) {
        detail::apply_frequency_retouch(
            image,
            source,
            coverage,
            stroke_lower_x,
            stroke_lower_y,
            coverage_width,
            coverage_height,
            source_mapping,
            stroke.strength,
            stroke.mode == SpotRepairMode::texture,
            stroke.frequency_radius
        );
        return;
    }
    if (stroke.mode != SpotRepairMode::clone) {
        detail::apply_texture_heal(
            image,
            source,
            coverage,
            stroke_lower_x,
            stroke_lower_y,
            coverage_width,
            coverage_height,
            source_mapping,
            stroke.strength,
            stroke.mode == SpotRepairMode::heal_structure
        );
        return;
    }
    for (std::int64_t y = stroke_lower_y; y <= stroke_upper_y; ++y) {
        for (std::int64_t x = stroke_lower_x; x <= stroke_upper_x; ++x) {
            const double alpha = stroke.strength
                                 * coverage
                                     [static_cast<std::size_t>(y - stroke_lower_y) * coverage_width
                                      + static_cast<std::size_t>(x - stroke_lower_x)];
            if (alpha <= 0.0) {
                continue;
            }
            const std::size_t sample =
                sample_index(image, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const std::array<double, rgb_channels> replacement = sample_bilinear(
                source,
                source_mapping.source_x(static_cast<double>(x), static_cast<double>(y)),
                source_mapping.source_y(static_cast<double>(x), static_cast<double>(y))
            );
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const double value = std::fma(
                    alpha,
                    replacement[channel] - static_cast<double>(source.samples[sample + channel]),
                    static_cast<double>(source.samples[sample + channel])
                );
                if (!std::isfinite(value)
                    || value < static_cast<double>(std::numeric_limits<float>::lowest())
                    || value > static_cast<double>(std::numeric_limits<float>::max())) {
                    invalid_retouch("continuous stroke produced an invalid RGB sample");
                }
                image.samples[sample + channel] = static_cast<float>(value);
            }
        }
    }
}

} // namespace

void validate_spot_heal(const SpotHealAdjustment& adjustment) {
    if (adjustment.spots.empty() && adjustment.strokes.empty()) {
        invalid_retouch("must contain at least one target or continuous stroke");
    }
    if (adjustment.spots.size() > maximum_spot_heal_targets) {
        invalid_retouch("must contain no more than 64 legacy targets");
    }
    if (adjustment.strokes.size() > maximum_retouch_strokes) {
        invalid_retouch("must contain no more than 64 continuous strokes");
    }
    for (const auto& target : adjustment.spots) {
        if (target.frequency_radius < 2 || target.frequency_radius > 32
            || !std::isfinite(target.center_x) || !std::isfinite(target.center_y)
            || target.center_x < 0.0 || target.center_x > 1.0 || target.center_y < 0.0
            || target.center_y > 1.0
            || !valid_retouch_properties(
                target.radius_level_zero_pixels,
                target.mode,
                target.source_offset_x_radii,
                target.source_offset_y_radii,
                target.source_rotation_degrees,
                target.source_scale,
                target.feather,
                target.strength
            )) {
            invalid_retouch("target coordinates or radius are outside the supported range");
        }
    }
    for (const auto& stroke : adjustment.strokes) {
        if (stroke.frequency_radius < 2 || stroke.frequency_radius > 32 || stroke.points.empty()
            || stroke.points.size() > maximum_retouch_stroke_points
            || !valid_retouch_properties(
                stroke.radius_level_zero_pixels,
                stroke.mode,
                stroke.source_offset_x_radii,
                stroke.source_offset_y_radii,
                stroke.source_rotation_degrees,
                stroke.source_scale,
                stroke.feather,
                stroke.strength
            )) {
            invalid_retouch("continuous stroke properties are outside the supported range");
        }
        for (const auto point : stroke.points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0 || point.x > 1.0
                || point.y < 0.0 || point.y > 1.0) {
                invalid_retouch("continuous stroke points are outside the normalized image");
            }
        }
    }
}

void apply_spot_heal(
    FloatRgbImage& image,
    const SpotHealAdjustment& adjustment,
    const AdjustmentExecutionContext context
) {
    validate_spot_heal(adjustment);
    const Dimensions full = execution_full_dimensions(image, context);
    for (const auto& target : adjustment.spots) {
        apply_target(image, target, context, full);
    }
    for (const auto& stroke : adjustment.strokes) {
        apply_stroke(image, stroke, context, full);
    }
}

} // namespace shadow::image

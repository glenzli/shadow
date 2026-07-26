#include "preview_diagnostics.hpp"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

inline constexpr auto sensor_highlight_clipped = static_cast<unsigned char>(1U << 0U);
inline constexpr auto sensor_shadow_clipped = static_cast<unsigned char>(1U << 1U);
inline constexpr std::uint64_t maximum_scope_samples = 262'144U;
inline constexpr std::uint32_t point_color_density_scale = 256U;
// These Cb/Cr coordinates are the same direction as the drawn skin guide in
// EditHistogram: `cb` maps horizontally and `cr` maps vertically upward.
inline constexpr double skin_guide_cb = -0.24;
inline constexpr double skin_guide_cr = 0.31;
inline constexpr double minimum_centroid_chroma = 0.015;

[[nodiscard]] qsizetype scope_sample_count() noexcept {
    return static_cast<qsizetype>(preview_scope_grid_size) * preview_scope_grid_size;
}

[[nodiscard]] int scope_index(const int x, const int y) noexcept {
    return y * preview_scope_grid_size + x;
}

[[nodiscard]] int scope_coordinate(
    const int coordinate,
    const int source_extent
) noexcept {
    if (source_extent <= 1) {
        return 0;
    }
    return std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(coordinate) * (preview_scope_grid_size - 1)
                / static_cast<double>(source_extent - 1)
        )),
        0,
        preview_scope_grid_size - 1
    );
}

[[nodiscard]] int scope_value_coordinate(const double value) noexcept {
    return std::clamp(
        static_cast<int>(std::lround(value * (preview_scope_grid_size - 1))),
        0,
        preview_scope_grid_size - 1
    );
}

[[nodiscard]] int sampling_stride(const QSize dimensions) noexcept {
    const auto width = static_cast<std::uint64_t>(std::max(1, dimensions.width()));
    const auto height = static_cast<std::uint64_t>(std::max(1, dimensions.height()));
    const auto pixel_count = width * height;
    if (pixel_count <= maximum_scope_samples) {
        return 1;
    }
    return std::max(1, static_cast<int>(std::ceil(std::sqrt(
        static_cast<double>(pixel_count) / static_cast<double>(maximum_scope_samples)
    ))));
}

[[nodiscard]] double smoothstep(
    const double edge0,
    const double edge1,
    const double value
) noexcept {
    if (edge0 >= edge1) {
        return value >= edge1 ? 1.0 : 0.0;
    }
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] bool valid_point_color_qualifier(
    const PreviewScopeHueQualifier& qualifier
) noexcept {
    return std::isfinite(qualifier.center_degrees)
        && qualifier.center_degrees >= 0.0
        && qualifier.center_degrees <= 360.0
        && std::isfinite(qualifier.width_degrees)
        && qualifier.width_degrees >= 1.0
        && qualifier.width_degrees <= 180.0
        && std::isfinite(qualifier.softness)
        && qualifier.softness >= 0.0
        && qualifier.softness <= 1.0;
}

[[nodiscard]] double srgb_to_linear(const double value) noexcept {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double point_color_selection_weight(
    const double red,
    const double green,
    const double blue,
    const PreviewScopeHueQualifier& qualifier
) noexcept {
    // This is intentionally the same hue-confidence and soft-edge selector
    // used by the perceptual Point Color stage. The scope samples display RGB,
    // so it remains a display-referred diagnostic rather than a second grade.
    const double linear_red = srgb_to_linear(red);
    const double linear_green = srgb_to_linear(green);
    const double linear_blue = srgb_to_linear(blue);
    const double l = std::cbrt(
        0.4122214708 * linear_red + 0.5363325363 * linear_green + 0.0514459929 * linear_blue
    );
    const double m = std::cbrt(
        0.2119034982 * linear_red + 0.6806995451 * linear_green + 0.1073969566 * linear_blue
    );
    const double s = std::cbrt(
        0.0883024619 * linear_red + 0.2817188376 * linear_green + 0.6299787005 * linear_blue
    );
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    const double oklab_l = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double chroma = std::hypot(a, b);
    const double confidence = smoothstep(
        0.002,
        0.02,
        chroma / std::max(1.0e-6, std::abs(oklab_l))
    );
    if (confidence == 0.0) {
        return 0.0;
    }
    double hue = std::atan2(b, a) * 180.0 / std::numbers::pi;
    if (hue < 0.0) {
        hue += 360.0;
    }
    const double distance = std::abs(std::remainder(hue - qualifier.center_degrees, 360.0));
    const double feather = qualifier.width_degrees * qualifier.softness;
    const double range_weight = feather == 0.0
        ? (distance <= qualifier.width_degrees ? 1.0 : 0.0)
        : 1.0 - smoothstep(
            qualifier.width_degrees - feather,
            qualifier.width_degrees,
            distance
        );
    return confidence * range_weight;
}

[[nodiscard]] std::uint32_t point_color_density(const double selection_weight) noexcept {
    return std::max(
        1U,
        static_cast<std::uint32_t>(std::lround(
            std::clamp(selection_weight, 0.0, 1.0) * point_color_density_scale
        ))
    );
}

[[nodiscard]] bool matching_sensor_mask(
    const QSize preview_dimensions,
    const PreviewSensorClippingMask& mask
) noexcept {
    const auto expected = static_cast<quint64>(preview_dimensions.width())
        * static_cast<quint64>(preview_dimensions.height());
    return mask.available && preview_dimensions.isValid() && mask.dimensions == preview_dimensions
        && expected <= static_cast<quint64>(std::numeric_limits<qsizetype>::max())
        && mask.samples.size() == static_cast<qsizetype>(expected);
}

void paint_stripe(
    unsigned char* const pixel,
    const bool highlight,
    const bool shadow
) noexcept {
    if (highlight && shadow) {
        pixel[0] = 214;
        pixel[1] = 89;
        pixel[2] = 255;
    } else if (highlight) {
        pixel[0] = 255;
        pixel[1] = 66;
        pixel[2] = 84;
    } else {
        pixel[0] = 51;
        pixel[1] = 155;
        pixel[2] = 255;
    }
    pixel[3] = 186;
}

[[nodiscard]] QImage sensor_clipping_overlay(
    const QSize preview_dimensions,
    const PreviewSensorClippingMask& mask
) noexcept {
    QImage overlay(preview_dimensions, QImage::Format_RGBA8888);
    if (overlay.isNull()) {
        return {};
    }
    overlay.fill(Qt::transparent);
    for (int y = 0; y < preview_dimensions.height(); ++y) {
        auto* const line = overlay.scanLine(y);
        for (int x = 0; x < preview_dimensions.width(); ++x) {
            const auto index = static_cast<qsizetype>(y) * preview_dimensions.width() + x;
            const auto flags = static_cast<unsigned char>(mask.samples.at(index));
            const bool highlight = (flags & sensor_highlight_clipped) != 0U;
            const bool shadow = (flags & sensor_shadow_clipped) != 0U;
            if ((!highlight && !shadow) || (((x / 5) + (y / 5)) & 1) != 0) {
                continue;
            }
            paint_stripe(line + (x * 4), highlight, shadow);
        }
    }
    return overlay;
}

[[nodiscard]] QImage display_endpoint_overlay(
    const QSize preview_dimensions,
    const QByteArray& encoded_preview
) noexcept {
    QImage source = QImage::fromData(encoded_preview, "JPEG");
    if (source.isNull()) {
        return {};
    }
    source = source.convertToFormat(QImage::Format_RGBA8888);
    if (source.isNull() || source.size() != preview_dimensions) {
        return {};
    }
    QImage overlay(preview_dimensions, QImage::Format_RGBA8888);
    if (overlay.isNull()) {
        return {};
    }
    overlay.fill(Qt::transparent);
    for (int y = 0; y < source.height(); ++y) {
        const auto* const source_line = source.constScanLine(y);
        auto* const overlay_line = overlay.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const auto* const source_pixel = source_line + (x * 4);
            const int red = source_pixel[0];
            const int green = source_pixel[1];
            const int blue = source_pixel[2];
            const bool highlight = std::max({red, green, blue}) >= 252;
            const bool shadow = std::max({red, green, blue}) <= 3;
            if ((!highlight && !shadow) || (((x / 5) + (y / 5)) & 1) != 0) {
                continue;
            }
            paint_stripe(overlay_line + (x * 4), highlight, shadow);
        }
    }
    return overlay;
}

} // namespace

PreviewDisplayScopeAnalysis analyze_display_scope(
    const QImage& preview,
    const std::optional<PreviewScopeHueQualifier> point_color_qualifier
) noexcept {
    try {
        if (preview.isNull() || !preview.size().isValid()) {
            return {};
        }
        if (point_color_qualifier.has_value()
            && !valid_point_color_qualifier(*point_color_qualifier)) {
            return {};
        }
        const QImage source = preview.convertToFormat(QImage::Format_RGBA8888);
        if (source.isNull()) {
            return {};
        }

        PreviewDisplayScopeAnalysis analysis;
        analysis.point_color_qualified = point_color_qualifier.has_value();
        analysis.source_dimensions = source.size();
        const qsizetype count = scope_sample_count();
        analysis.waveform.fill(0U, count);
        analysis.parade_red.fill(0U, count);
        analysis.parade_green.fill(0U, count);
        analysis.parade_blue.fill(0U, count);
        analysis.vectorscope.fill(0U, count);
        double centroid_cb_sum = 0.0;
        double centroid_cr_sum = 0.0;
        double centroid_weight_sum = 0.0;

        const int stride = sampling_stride(source.size());
        for (int y = 0; y < source.height(); y += stride) {
            const auto* const line = source.constScanLine(y);
            for (int x = 0; x < source.width(); x += stride) {
                const auto* const pixel = line + x * 4;
                const double red = static_cast<double>(pixel[0]) / 255.0;
                const double green = static_cast<double>(pixel[1]) / 255.0;
                const double blue = static_cast<double>(pixel[2]) / 255.0;
                const double selection_weight = point_color_qualifier.has_value()
                    ? point_color_selection_weight(red, green, blue, *point_color_qualifier)
                    : 1.0;
                ++analysis.sampled_pixels;
                if (selection_weight <= 0.0) {
                    continue;
                }
                ++analysis.matched_pixels;
                const std::uint32_t density = point_color_qualifier.has_value()
                    ? point_color_density(selection_weight)
                    : 1U;
                const int scope_x = scope_coordinate(x, source.width());
                const double luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;

                analysis.waveform[scope_index(
                    scope_x,
                    preview_scope_grid_size - 1 - scope_value_coordinate(luma)
                )] += density;
                analysis.parade_red[scope_index(
                    scope_x,
                    preview_scope_grid_size - 1 - scope_value_coordinate(red)
                )] += density;
                analysis.parade_green[scope_index(
                    scope_x,
                    preview_scope_grid_size - 1 - scope_value_coordinate(green)
                )] += density;
                analysis.parade_blue[scope_index(
                    scope_x,
                    preview_scope_grid_size - 1 - scope_value_coordinate(blue)
                )] += density;

                // Rec.709 Y'CbCr chroma axes provide a familiar vectorscope
                // projection for the display-referred JPEG. The correction
                // pipeline remains perceptual/OKLch; this is diagnostics only.
                const double cb = (blue - luma) / (2.0 * (1.0 - 0.0722));
                const double cr = (red - luma) / (2.0 * (1.0 - 0.2126));
                centroid_cb_sum += selection_weight * cb;
                centroid_cr_sum += selection_weight * cr;
                centroid_weight_sum += selection_weight;
                analysis.vectorscope[scope_index(
                    scope_value_coordinate(cb + 0.5),
                    scope_value_coordinate(0.5 - cr)
                )] += density;
            }
        }
        if (centroid_weight_sum > 0.0) {
            const double centroid_cb = centroid_cb_sum / centroid_weight_sum;
            const double centroid_cr = centroid_cr_sum / centroid_weight_sum;
            if (std::hypot(centroid_cb, centroid_cr) >= minimum_centroid_chroma) {
                analysis.has_vectorscope_centroid = true;
                analysis.vectorscope_centroid_cb = centroid_cb;
                analysis.vectorscope_centroid_cr = centroid_cr;
                const double centroid_degrees = std::atan2(centroid_cr, centroid_cb)
                    * 180.0 / std::numbers::pi;
                const double guide_degrees = std::atan2(skin_guide_cr, skin_guide_cb)
                    * 180.0 / std::numbers::pi;
                analysis.skin_guide_deviation_degrees = std::remainder(
                    centroid_degrees - guide_degrees,
                    360.0
                );
            }
        }
        analysis.available = analysis.sampled_pixels > 0;
        return analysis;
    } catch (...) {
        // Preview diagnostics must never make a valid preview unavailable.
        return {};
    }
}

PreviewDisplayScopeAnalysis analyze_display_scope(
    const QByteArray& encoded_preview,
    const std::optional<PreviewScopeHueQualifier> point_color_qualifier
) noexcept {
    try {
        return analyze_display_scope(
            QImage::fromData(encoded_preview, "JPEG"),
            point_color_qualifier
        );
    } catch (...) {
        return {};
    }
}

QImage make_clipping_zebra_overlay(
    const QSize preview_dimensions,
    const QByteArray& encoded_preview,
    const PreviewSensorClippingMask& sensor_mask
) noexcept {
    try {
        if (!preview_dimensions.isValid()) {
            return {};
        }
        if (matching_sensor_mask(preview_dimensions, sensor_mask)) {
            return sensor_clipping_overlay(preview_dimensions, sensor_mask);
        }
        return display_endpoint_overlay(preview_dimensions, encoded_preview);
    } catch (...) {
        // Diagnostics are supplemental. A malformed optional mask must never hide the photo.
        return {};
    }
}

#include "preview_diagnostics.hpp"

#include <QtGlobal>

#include <algorithm>
#include <limits>

namespace {

inline constexpr auto sensor_highlight_clipped = static_cast<unsigned char>(1U << 0U);
inline constexpr auto sensor_shadow_clipped = static_cast<unsigned char>(1U << 1U);

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

#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>

#include <cstdint>

// One immutable clipping diagnostic returned with an edit preview. For RAW sources it is a
// compact sensor-domain mask; raster sources intentionally leave it unavailable and use the
// display-endpoint fallback in the implementation.
struct PreviewSensorClippingMask final {
    bool available = false;
    QSize dimensions;
    QByteArray samples;
    std::uint64_t highlight_pixel_count = 0;
    std::uint64_t shadow_pixel_count = 0;
};

// Produces the translucent editor overlay. When a valid sensor mask is supplied it is
// authoritative: red means an irrecoverably saturated RAW sample and blue means all samples in
// the local source region are at the calibrated black floor. JPEG/HEIF fall back to a clearly
// weaker display-endpoint warning, because they have no RAW headroom information to report.
[[nodiscard]] QImage make_clipping_zebra_overlay(
    QSize preview_dimensions,
    const QByteArray& encoded_preview,
    const PreviewSensorClippingMask& sensor_mask
) noexcept;

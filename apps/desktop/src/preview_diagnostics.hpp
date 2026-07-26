#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QVector>

#include <cstdint>
#include <optional>

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

// Display-referred scopes deliberately analyse the encoded preview that the
// user sees. They complement, rather than replace, the renderer's scene-linear
// histogram and clipping analysis. Keeping both contracts explicit prevents a
// JPEG/display value from being mistaken for RAW exposure evidence.
inline constexpr int preview_scope_grid_size = 128;

// A diagnostic-only mirror of one Point Color selector. It never changes the
// preview: it merely lets a scope show the pixels that the selected range can
// affect, using the same OKLCh hue-width-softness contract as Point Color.
struct PreviewScopeHueQualifier final {
    double center_degrees = 0.0;
    double width_degrees = 30.0;
    double softness = 0.5;
};

struct PreviewDisplayScopeAnalysis final {
    bool available = false;
    bool point_color_qualified = false;
    QSize source_dimensions;
    std::uint64_t sampled_pixels = 0;
    std::uint64_t matched_pixels = 0;
    bool has_vectorscope_centroid = false;
    double vectorscope_centroid_cb = 0.0;
    double vectorscope_centroid_cr = 0.0;
    double skin_guide_deviation_degrees = 0.0;
    QVector<std::uint32_t> waveform;
    QVector<std::uint32_t> parade_red;
    QVector<std::uint32_t> parade_green;
    QVector<std::uint32_t> parade_blue;
    QVector<std::uint32_t> vectorscope;
};

[[nodiscard]] PreviewDisplayScopeAnalysis analyze_display_scope(
    const QImage& preview,
    std::optional<PreviewScopeHueQualifier> point_color_qualifier = std::nullopt
) noexcept;

[[nodiscard]] PreviewDisplayScopeAnalysis analyze_display_scope(
    const QByteArray& encoded_preview,
    std::optional<PreviewScopeHueQualifier> point_color_qualifier = std::nullopt
) noexcept;

// Produces the translucent editor overlay. When a valid sensor mask is supplied it is
// authoritative: red means an irrecoverably saturated RAW sample and blue means all samples in
// the local source region are at the calibrated black floor. JPEG/HEIF fall back to a clearly
// weaker display-endpoint warning, because they have no RAW headroom information to report.
[[nodiscard]] QImage make_clipping_zebra_overlay(
    QSize preview_dimensions,
    const QByteArray& encoded_preview,
    const PreviewSensorClippingMask& sensor_mask
) noexcept;

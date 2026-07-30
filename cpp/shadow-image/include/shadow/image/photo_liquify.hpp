#pragma once

#include <shadow/image/decoder_types.hpp>

#include <compare>
#include <cstddef>
#include <vector>

namespace shadow::image {

struct FloatRgbImage;

inline constexpr std::size_t maximum_photo_liquify_strokes = 128U;
inline constexpr std::size_t maximum_photo_liquify_points_per_stroke = 2'048U;
inline constexpr std::size_t maximum_prepared_photo_liquify_stamps = 65'536U;

/// One pressure-bearing sample in normalized uncropped image-edge coordinates.
struct PhotoLiquifyPoint final {
    double x = 0.0;
    double y = 0.0;
    double pressure = 1.0;

    auto operator<=>(const PhotoLiquifyPoint&) const = default;
};

/// One authored push-brush gesture.
///
/// Radius is a fraction of the source image's shorter edge. Strength and
/// hardness are normalized. Authored paths remain resolution-independent;
/// preparation lowers them into pixel-space stamps for a specific proxy,
/// detail raster, or export.
struct PhotoLiquifyPushStroke final {
    std::vector<PhotoLiquifyPoint> points;
    double radius = 0.0;
    double strength = 1.0;
    double hardness = 0.5;

    auto operator<=>(const PhotoLiquifyPushStroke&) const = default;
};

/// The optional node payload after the Recipe layer has established presence.
///
/// A value of this type is non-empty and belongs to one photograph. It has no
/// shared identity and is executed before the mandatory Canvas node.
struct PhotoLiquify final {
    std::vector<PhotoLiquifyPushStroke> strokes;

    auto operator<=>(const PhotoLiquify&) const = default;
};

/// One resolution-specific local translation used by the inverse sampler.
struct PreparedPhotoLiquifyStamp final {
    double center_x = 0.0;
    double center_y = 0.0;
    double displacement_x = 0.0;
    double displacement_y = 0.0;
    double radius = 0.0;
    double hardness = 0.0;

    auto operator<=>(const PreparedPhotoLiquifyStamp&) const = default;
};

/// Rebuildable execution detail for one raster size.
///
/// `maximum_displacement_pixels` is a conservative source-preimage expansion
/// bound. The first tile integration may use it globally; a later spatial
/// index can tighten the bound without changing authored Recipe data.
struct PreparedPhotoLiquify final {
    Dimensions source_dimensions;
    std::vector<PreparedPhotoLiquifyStamp> stamps;
    double maximum_displacement_pixels = 0.0;

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const PreparedPhotoLiquify&) const = default;
};

void validate_photo_liquify(const PhotoLiquify& liquify);

/// Deterministically resamples authored paths into bounded pixel-space stamps.
[[nodiscard]] PreparedPhotoLiquify prepare_photo_liquify(
    Dimensions source_dimensions,
    const PhotoLiquify& liquify
);

/// Applies all prepared gestures through one inverse-map bilinear sample.
[[nodiscard]] FloatRgbImage apply_photo_liquify(
    const FloatRgbImage& source,
    const PreparedPhotoLiquify& liquify
);

} // namespace shadow::image

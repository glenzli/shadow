#pragma once

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace shadow::image {

inline constexpr std::string_view raw_foundation_model_identity =
    "rawnind-public-bayer-release-5.6.0";
inline constexpr std::string_view raw_foundation_implementation_revision =
    "rawnind-public-bayer-foundation-20260731.1";

/// Path-free identity of one verified AI RAW foundation.
///
/// Rust verifies the complete `.shadowrawf` framing, payload, model package,
/// source bytes, and digests before constructing this view. Native code repeats
/// the fixed identity and lowercase-SHA shape checks before pixels can enter
/// camera development.
struct RawFoundationProvenance final {
    std::string source_sha256;
    std::string artifact_file_sha256;
    std::string cache_key_sha256;
    std::string model_identity;
    std::string implementation_revision;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string cache_identity() const;
};

/// Borrowed, fully verified interleaved linear-camera-RGB pixels.
///
/// `crop_top` and `crop_left` are the zero-or-one active-sensor offsets used
/// by the public RawNIND preprocessing contract to canonicalize Bayer phase.
/// The view is consumed synchronously; it never retains a path or borrows
/// beyond one preparation call.
struct RawFoundationCameraRgbView final {
    Dimensions dimensions;
    std::uint32_t crop_top = 0U;
    std::uint32_t crop_left = 0U;
    std::span<const float> samples;
    RawFoundationProvenance provenance;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool matches_source(const RawFrameDescriptor& descriptor) const noexcept;
};

struct DevelopedRawFoundation final {
    SceneLinearRgbFrame scene_linear;
    Dimensions camera_rgb_dimensions;
    bool bounded_preview = false;
    std::string cache_identity;

    [[nodiscard]] bool valid() const noexcept;
};

/// Applies the source-bound camera-to-working transform to a verified AI
/// foundation. A preview edge produces a bounded camera-RGB raster before the
/// linear transform; full detail and export pass `std::nullopt`.
///
/// This stage deliberately does not apply DCP post-matrix rendering, optics,
/// Recipe adjustments, highlight reconstruction, or display encoding. Those
/// remain the existing downstream owners.
[[nodiscard]] DevelopedRawFoundation develop_raw_foundation(
    const RawFoundationCameraRgbView& foundation,
    const RawFrameDescriptor& source_descriptor,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge = std::nullopt
);

} // namespace shadow::image

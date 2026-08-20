#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>

namespace shadow::image {

struct RawFrameLinearTransform;

namespace detail {

using CameraRgb = std::array<float, 3U>;
using RawCfaFootprint = std::array<float, 4U>;

// Camera-linear reconstruction carries both WB-applied camera RGB and a per-site CFA highlight
// risk. The risk is formed from calibrated physical headroom before white balance; a bounded
// preview retains the highest continuous risk from each CFA site, so a narrow sensor plateau is
// not diluted into a false low-risk colour during downsampling.
struct CameraRgbSample final {
    CameraRgb values{};
    RawCfaFootprint cfa_highlight_risk{};
};

struct BayerAreaSamplingGrid final {
    Dimensions target_dimensions;
    double scale_x = 0.0;
    double scale_y = 0.0;
};

void validate_bayer_frame(const RawFrame& frame, const char* operation);

// Same-colour 3x3 reconstruction used by both the materialized reference path and the fused
// renderer. The float result is an intentional numeric boundary retained for exact parity.
[[nodiscard]] CameraRgb bilinear_camera_rgb_at(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    const RawFrameLinearTransform* transform = nullptr
);

[[nodiscard]] CameraRgbSample bilinear_camera_rgb_sample_at(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    const RawFrameLinearTransform* transform = nullptr
);

// Detail/export reconstruction uses a directional green estimate plus local
// colour-difference interpolation. This stays in the sensor domain; clipping
// is projected independently from the immutable RAW plane for a later,
// explicit rendering or diagnostic stage.
[[nodiscard]] CameraRgbSample edge_aware_camera_rgb_sample_at(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    const RawFrameLinearTransform* transform = nullptr
);

// Precomputes scale ratios once per preview render. Each sampling call then integrates the exact
// active-sensor footprint represented by its target pixel, independently per CFA colour.
[[nodiscard]] BayerAreaSamplingGrid
make_bayer_area_sampling_grid(const RawFrame& frame, Dimensions target_dimensions);

[[nodiscard]] CameraRgb area_camera_rgb_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    std::uint32_t target_x,
    std::uint32_t target_y,
    const RawFrameLinearTransform* transform = nullptr
);

[[nodiscard]] CameraRgbSample area_camera_rgb_sample_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    std::uint32_t target_x,
    std::uint32_t target_y,
    const RawFrameLinearTransform* transform = nullptr
);

} // namespace detail

} // namespace shadow::image

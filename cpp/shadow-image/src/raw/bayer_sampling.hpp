#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>

namespace shadow::image::detail {

using CameraRgb = std::array<float, 3U>;

// Camera-linear reconstruction carries only measured, normalized camera RGB. Sensor clipping is
// projected separately from the immutable RAW plane, so source development never alters colour
// merely because one CFA channel reached its calibrated ceiling.
struct CameraRgbSample final {
    CameraRgb values{};
};

struct BayerAreaSamplingGrid final {
    Dimensions target_dimensions;
    double scale_x = 0.0;
    double scale_y = 0.0;
};

void validate_bayer_frame(const RawFrame& frame, const char* operation);

// Same-colour 3x3 reconstruction used by both the materialized reference path and the fused
// renderer. The float result is an intentional numeric boundary retained for exact parity.
[[nodiscard]] CameraRgb
bilinear_camera_rgb_at(const RawFrame& frame, std::uint32_t raw_x, std::uint32_t raw_y);

[[nodiscard]] CameraRgbSample
bilinear_camera_rgb_sample_at(const RawFrame& frame, std::uint32_t raw_x, std::uint32_t raw_y);

// Detail/export reconstruction uses a directional green estimate plus local
// colour-difference interpolation. This stays in the sensor domain; clipping
// is projected independently from the immutable RAW plane for a later,
// explicit rendering or diagnostic stage.
[[nodiscard]] CameraRgbSample
edge_aware_camera_rgb_sample_at(const RawFrame& frame, std::uint32_t raw_x, std::uint32_t raw_y);

// Precomputes scale ratios once per preview render. Each sampling call then integrates the exact
// active-sensor footprint represented by its target pixel, independently per CFA colour.
[[nodiscard]] BayerAreaSamplingGrid
make_bayer_area_sampling_grid(const RawFrame& frame, Dimensions target_dimensions);

[[nodiscard]] CameraRgb area_camera_rgb_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    std::uint32_t target_x,
    std::uint32_t target_y
);

[[nodiscard]] CameraRgbSample area_camera_rgb_sample_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    std::uint32_t target_x,
    std::uint32_t target_y
);

} // namespace shadow::image::detail

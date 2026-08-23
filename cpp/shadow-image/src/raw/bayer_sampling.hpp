#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>

namespace shadow::image {

struct RawFrameLinearTransform;

namespace detail {

using CameraRgb = std::array<float, 3U>;
// Editable RAW keeps the exact CFA white-balance scale before demosaic but retains the resulting
// float headroom. Only a sample that has reached its physical sensor white may be limited at the
// white point, so an unsaturated sample amplified above one by white balance stays editable.
// The policy is explicit so diagnostic/AI routes retain their existing measured-source contract.
struct BayerCfaSamplingPolicy final {
    float white_balance_scale = 1.0F;
    bool cap_physical_sensor_white = false;
    // Opt-in repair mode only. It spatially feathers the physical-white evidence before
    // neutralization, deliberately sacrificing uncertain highlight chroma to avoid a hard
    // false-colour boundary. It never invents a colour or a luminance value.
    bool feather_highlight_chroma_neutralization = false;
};

[[nodiscard]] BayerCfaSamplingPolicy
editable_raw_cfa_sampling_policy(const RawFrameLinearTransform& transform) noexcept;

[[nodiscard]] BayerCfaSamplingPolicy
aggressive_highlight_repair_cfa_sampling_policy(const RawFrameLinearTransform& transform) noexcept;

// Camera-linear reconstruction carries CFA-white-balanced samples after the selected source
// policy. Each RGB entry retains both continuous headroom evidence and the exact fraction of its
// contributing CFA sites that reached physical sensor white. The sampler preserves those measured
// values and exports the evidence sidecar used by the local, one-sided opposed-colour repair before
// the camera matrix and by the later continuous scene-linear highlight surface.
struct CameraRgbSample final {
    CameraRgb values{};
    CameraRgb highlight_channel_evidence{};
    CameraRgb physical_white_coverage{};
    // Source-local blend weight for discarding an unmeasured neutral-highlight chroma ratio. It
    // is derived from the continuous per-channel CFA evidence and never survives as a preview map.
    float highlight_chroma_neutralization = 0.0F;
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
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
);

[[nodiscard]] CameraRgbSample bilinear_camera_rgb_sample_at(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
);

// Detail/export reconstruction uses a directional green estimate plus local
// colour-difference interpolation. It remains in the sensor domain and carries
// physical-white evidence through the same source-stage treatment as preview.
[[nodiscard]] CameraRgbSample edge_aware_camera_rgb_sample_at(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
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
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
);

[[nodiscard]] CameraRgbSample area_camera_rgb_sample_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    std::uint32_t target_x,
    std::uint32_t target_y,
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
);

} // namespace detail

} // namespace shadow::image

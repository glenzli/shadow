#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace shadow::image {

struct RawFrameLinearTransform;

namespace detail {

using CameraRgb = std::array<float, 3U>;

enum class CfaTerminalHighlightAdmission : std::uint8_t {
    // Offline diagnostic: admit reconstruction at the provider-calibrated response frontier.
    // This intentionally broader branch remains available to the oracle threshold ablation.
    linear_response_limit = 0,
    // Production default: only the exact photosite whose stored code reaches per-phase physical
    // white may be replaced. Response evidence still informs highlight-risk confidence without
    // expanding reconstruction write ownership.
    physical_white,
};

// Editable RAW keeps the exact CFA white-balance scale before demosaic but retains the resulting
// float headroom. Only a sample that has reached its physical sensor white may be limited at the
// white point, so an unsaturated sample amplified above one by white balance stays editable.
// The policy is explicit so diagnostic/AI routes retain their existing measured-source contract.
struct BayerCfaSamplingPolicy final {
    float white_balance_scale = 1.0F;
    bool cap_physical_sensor_white = false;
    CfaTerminalHighlightAdmission terminal_highlight_admission =
        CfaTerminalHighlightAdmission::physical_white;
    // Physical-white detection and topology remain active, but a terminal photosite may retain its
    // CFA white-balance-scaled fp32 value instead of being projected onto a common 1.0 ceiling.
    // Editable RAW uses this scene-referred domain; diagnostics may disable it to compare the old
    // common-white projection without changing clipping evidence or reconstruction policy.
    bool preserve_terminal_white_balance_headroom = false;
    // A bounded diagnostic may admit physical-white WB headroom only when all CFA colours in the
    // local reconstruction footprint are terminal. Production keeps this false: projecting an
    // isolated terminal phase to a common ceiling creates a Bayer-aligned spatial discontinuity.
    bool require_shared_terminal_headroom = false;
    // Production editable policies enable the source-local opposed reconstruction. Diagnostics may
    // disable it while retaining identical white-balance and physical-white normalization so the
    // post-demosaic delta has a controlled baseline.
    bool reconstruct_terminal_highlights = false;
    // Opt-in repair mode only. It spatially feathers the physical-white evidence before
    // neutralization, deliberately sacrificing uncertain highlight chroma to avoid a hard
    // false-colour boundary. It never invents a colour or a luminance value.
    bool feather_highlight_chroma_neutralization = false;
};

// Source-local opposed reconstruction for one CFA photosite. Keeping the measured value,
// darktable-style opposed reference, and final one-sided result together lets diagnostics measure
// the exact pre-demosaic delta without copying or reimplementing the production algorithm.
struct CfaOpposedHighlightSample final {
    float measured = 0.0F;
    float opposed_reference = 0.0F;
    float reconstructed = 0.0F;
    bool terminal_candidate = false;
};

struct CfaOpposedChrominanceCorrection final {
    CameraRgb offsets{};
    std::array<std::uint64_t, 3U> supporting_samples{};
    bool any_terminal_photosite = false;
};

// Bounded, source-bound support for darktable's second opposed term. Each record retains only a
// measured sensor value and the phase-resolved 3x3 neighbourhood needed to re-evaluate the
// chrominance residual under a different white balance. Building the model is O(sensor pixels)
// once; evaluating it is bounded independently of image dimensions and performs no image copy.
struct CfaOpposedChrominanceSupportRecord final {
    std::array<float, 4U> phase_totals{};
    std::array<std::uint8_t, 4U> phase_counts{};
    float measured_sensor = 0.0F;
    std::uint8_t measured_phase = 0U;
};

struct CfaOpposedChrominanceModel final {
    std::array<RawCfaColor, 4U> phase_colors{};
    std::array<std::vector<CfaOpposedChrominanceSupportRecord>, 3U> support_records;
    std::array<std::uint64_t, 3U> total_supporting_samples{};
    bool any_terminal_photosite = false;
};

[[nodiscard]] BayerCfaSamplingPolicy
editable_raw_cfa_sampling_policy(const RawFrameLinearTransform& transform) noexcept;

[[nodiscard]] BayerCfaSamplingPolicy
aggressive_highlight_repair_cfa_sampling_policy(const RawFrameLinearTransform& transform) noexcept;

[[nodiscard]] CfaOpposedHighlightSample opposed_highlight_cfa_sample_at(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
) noexcept;

// Estimates darktable's second opposed term from measured photosites surrounding factual terminal
// regions. This source-level diagnostic is deliberately separate from the point sampler so callers
// can cache the O(sensor-pixels) result instead of repeating it during demosaic or slider edits.
[[nodiscard]] CfaOpposedChrominanceCorrection estimate_opposed_highlight_chrominance_correction(
    const RawFrame& frame,
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {},
    std::uint32_t support_cell_stride = 1U
);

[[nodiscard]] CfaOpposedChrominanceModel build_opposed_highlight_chrominance_model(
    const RawFrame& frame,
    std::uint32_t support_cell_stride = 4U,
    std::size_t maximum_records_per_channel = 16'384U
);

[[nodiscard]] CfaOpposedChrominanceCorrection evaluate_opposed_highlight_chrominance_model(
    const CfaOpposedChrominanceModel& model,
    const RawFrameLinearTransform* transform = nullptr,
    BayerCfaSamplingPolicy sampling_policy = {}
) noexcept;

// Camera-linear reconstruction carries CFA-white-balanced samples after the selected source
// policy. Each RGB entry retains both continuous headroom evidence and the exact fraction of its
// contributing CFA sites that reached physical sensor white. The default sampler preserves those
// measured values and keeps the evidence diagnostic-only after the local, one-sided CFA repair;
// only the explicit aggressive policy may turn it into a later camera-RGB blend.
struct CameraRgbSample final {
    CameraRgb values{};
    CameraRgb highlight_channel_evidence{};
    CameraRgb physical_white_coverage{};
    // Explicit aggressive-mode blend weight for discarding an unmeasured neutral-highlight chroma
    // ratio. The default point-owned path leaves this at zero.
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

#pragma once

#include <shadow/image/raw_pipeline.hpp>

#include <cstdint>

namespace shadow::image::raw_pipeline_detail {

// Selects the immutable source representation that feeds camera development.
// This is intentionally a semantic boundary, not a storage abstraction: a CFA
// frame and AI camera RGB have different payload layouts and capabilities.
enum class SourceReconstructionKind : std::uint8_t {
    sensor_cfa,
    ai_camera_rgb,
};

// AI camera RGB is already a reconstructed source. Re-running Bayer-only
// denoise or Bayer highlight recovery after that boundary would be invalid and
// would make preview/detail/export disagree about the source stage.
[[nodiscard]] constexpr RawDevelopmentPlan source_reconstruction_effective_plan(
    RawDevelopmentPlan requested_plan,
    const SourceReconstructionKind reconstruction
) noexcept {
    if (reconstruction == SourceReconstructionKind::ai_camera_rgb) {
        requested_plan.noise_reduction = RawNoiseReductionIntent::disabled;
        requested_plan.highlight_recovery = RawHighlightRecoveryIntent::disabled;
    }
    return requested_plan;
}

[[nodiscard]] constexpr bool source_reconstruction_retains_sensor_cfa(
    const SourceReconstructionKind reconstruction
) noexcept {
    return reconstruction == SourceReconstructionKind::sensor_cfa;
}

} // namespace shadow::image::raw_pipeline_detail

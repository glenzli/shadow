#pragma once

#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/raw_foundation.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace shadow::image::raw_pipeline_detail {

// Selects the immutable source representation that feeds camera development.
// This is intentionally a semantic boundary, not a storage abstraction: a CFA
// frame and AI camera RGB have different payload layouts and capabilities.
enum class SourceReconstructionKind : std::uint8_t {
    sensor_cfa,
    ai_camera_rgb,
};

// These are owned, immutable source bases for a warm RAW preview. They remain
// distinct because source format, valid operations, and retained evidence are
// fundamentally different; the variant gives downstream source-integrity and
// camera-development stages one explicit handoff contract instead of a
// foundation-only side route.
struct SensorCfaSourceReconstructionBasis final {
    RawFrame denoised_frame;
    RawBayerDenoiseReceipt conventional_denoise;
    std::string combined_denoise_identity;
    SensorClippingMask sensor_clipping;
    HighlightChromaRiskMap highlight_chroma_risk;
};

struct AiCameraRgbSourceReconstructionBasis final {
    PreparedRawFoundationCameraRgb camera_rgb;
    // This remains derived from the immutable sensor frame, never inferred
    // from AI camera RGB, so both reconstruction kinds expose one exact source
    // evidence contract to the later highlight-integrity stage.
    SensorClippingMask sensor_clipping;
    HighlightChromaRiskMap highlight_chroma_risk;
};

using SourceReconstructionBasis =
    std::variant<SensorCfaSourceReconstructionBasis, AiCameraRgbSourceReconstructionBasis>;

[[nodiscard]] constexpr SourceReconstructionKind source_reconstruction_kind(
    const SourceReconstructionBasis& basis
) noexcept {
    return std::holds_alternative<SensorCfaSourceReconstructionBasis>(basis)
               ? SourceReconstructionKind::sensor_cfa
               : SourceReconstructionKind::ai_camera_rgb;
}

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

[[nodiscard]] constexpr bool source_reconstruction_retains_sensor_cfa(
    const SourceReconstructionBasis& basis
) noexcept {
    return source_reconstruction_retains_sensor_cfa(source_reconstruction_kind(basis));
}

[[nodiscard]] inline const SensorClippingMask& source_reconstruction_sensor_clipping(
    const SourceReconstructionBasis& basis
) noexcept {
    return std::visit(
        [](const auto& reconstruction) -> const SensorClippingMask& {
            return reconstruction.sensor_clipping;
        },
        basis
    );
}

[[nodiscard]] inline const HighlightChromaRiskMap& source_reconstruction_highlight_chroma_risk(
    const SourceReconstructionBasis& basis
) noexcept {
    return std::visit(
        [](const auto& reconstruction) -> const HighlightChromaRiskMap& {
            return reconstruction.highlight_chroma_risk;
        },
        basis
    );
}

// Prepares the exact owned AI source basis once. Both ordinary source
// materialization and warm preview rebinding use this helper, so the active
// sensor alignment, orientation, bounded-preview dimensions, and clipping
// evidence cannot drift between those consumers.
[[nodiscard]] inline AiCameraRgbSourceReconstructionBasis
prepare_ai_camera_rgb_source_reconstruction(
    const RawFoundationCameraRgbView& foundation,
    const RawFrame& source_frame,
    const std::optional<std::uint32_t> preview_max_edge
) {
    AiCameraRgbSourceReconstructionBasis basis{
        .camera_rgb =
            prepare_raw_foundation_camera_rgb(foundation, source_frame, preview_max_edge),
    };
    basis.sensor_clipping =
        project_sensor_clipping_mask(source_frame, basis.camera_rgb.dimensions);
    basis.highlight_chroma_risk =
        project_highlight_chroma_risk_map(source_frame, basis.camera_rgb.dimensions);
    return basis;
}

} // namespace shadow::image::raw_pipeline_detail

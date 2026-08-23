#pragma once

#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstdint>
#include <string_view>

namespace shadow::image::raw_pipeline_detail {

inline constexpr std::string_view clipped_highlight_surface_reconstruction_identity =
    "clipped-highlight=local-risk-guided-edge-aware-shoulder-v13";

// Continuous clipped-highlight treatment is the normal Shadow RAW source contract. The legacy
// aggressive value remains accepted as a wire/diagnostic alias; disabled is the only route that
// intentionally preserves the clipped projection's original chromaticity.
[[nodiscard]] constexpr bool
uses_clipped_highlight_surface_reconstruction(const RawHighlightRecoveryIntent intent) noexcept {
    return intent == RawHighlightRecoveryIntent::provider_default
           || intent == RawHighlightRecoveryIntent::aggressive;
}

struct ClippedHighlightReconstructionStats final {
    std::uint64_t clipped_pixel_count = 0U;
    std::uint64_t blended_pixel_count = 0U;
    Dimensions guide_dimensions;
};

// Softens source-unreliable highlight topology with bounded low-frequency local evidence. A
// reliable colour guide excludes clipped and CFA-risked samples, while a broader observed-light
// guide retains local luminance shape. Boundary chroma follows the reliable guide; a deep terminal
// core keeps its local colour when the two agree and is continuously desaturated when they do not.
// A continuous luminance bell softens the physical clipping frontier and the deep clipped surface
// adopts only the local observed-light trend, never distant texture. Edge gates keep either repair
// from crossing onto an adjacent dark subject, and far measured pixels remain unchanged. Remaining
// CFA risk decays by the actual blend instead of being cleared at the first touched pixel, so later
// recovery cannot reveal a second binary edge. The full-resolution frame is changed in place so
// source preparation does not allocate a second full-size RGB raster.
[[nodiscard]] ClippedHighlightReconstructionStats reconstruct_clipped_highlight_surface(
    SceneLinearRgbFrame& scene_linear,
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk
);

} // namespace shadow::image::raw_pipeline_detail

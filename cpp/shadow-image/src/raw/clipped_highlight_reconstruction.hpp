#pragma once

#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstdint>
#include <string_view>

namespace shadow::image::raw_pipeline_detail {

inline constexpr std::string_view clipped_highlight_surface_reconstruction_identity =
    "clipped-highlight-surface=low-frequency-push-pull-luminance-protected-v2";

// Continuous clipped-highlight treatment is the normal Shadow RAW source contract. The legacy
// aggressive value remains accepted as a wire/diagnostic alias; disabled is the only route that
// intentionally preserves the physical clipping contour.
[[nodiscard]] constexpr bool uses_clipped_highlight_surface_reconstruction(
    const RawHighlightRecoveryIntent intent
) noexcept {
    return intent == RawHighlightRecoveryIntent::provider_default
           || intent == RawHighlightRecoveryIntent::aggressive;
}

struct ClippedHighlightReconstructionStats final {
    std::uint64_t clipped_pixel_count = 0U;
    std::uint64_t blended_pixel_count = 0U;
    Dimensions guide_dimensions;
};

// Replaces only source-unreliable highlight topology with a bounded low-frequency estimate.
// The guide is reconstructed from measured scene-linear neighbours; it cannot fabricate texture.
// The clipped core retains nearly all measured scene luminance while adopting the reliable guide
// chromaticity, preventing strong downstream Highlight/White recovery from turning a warm clipped
// surface into a dark neutral island. A brightness-gated exterior chroma shoulder hides the binary
// contour without crossing an adjacent dark subject. The full-resolution frame is changed in
// place so source preparation does not allocate a second full-size RGB raster.
[[nodiscard]] ClippedHighlightReconstructionStats reconstruct_clipped_highlight_surface(
    SceneLinearRgbFrame& scene_linear,
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk
);

} // namespace shadow::image::raw_pipeline_detail

#pragma once

#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstdint>
#include <string_view>

namespace shadow::image::raw_pipeline_detail {

inline constexpr std::string_view clipped_highlight_surface_reconstruction_identity =
    "clipped-highlight-surface=risk-excluded-support-gated-push-pull-luma-shoulder-v4";

// Continuous clipped-highlight treatment is the normal Shadow RAW source contract. The legacy
// aggressive value remains accepted as a wire/diagnostic alias; disabled is the only route that
// intentionally preserves the physical clipping contour.
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

// Replaces only source-unreliable highlight topology with a bounded low-frequency estimate.
// The guide is reconstructed from measured scene-linear neighbours; it cannot fabricate texture.
// Spatial support distinguishes a coherent missing surface from an isolated CFA clip without
// modifying luminance on the discrete Bayer lattice. The low-frequency guide excludes samples
// whose CFA chroma is already marked unreliable, then the dense clipped core adopts that reliable
// guide chromaticity while retaining nearly all measured scene luminance. This prevents strong
// downstream Highlight/White recovery from turning a warm clipped surface into a dark neutral
// island. The first measured bright samples may lift toward the same guide, making one luminance
// shoulder across clipped and unclipped projection bins without lowering measured light. A
// luminance gate applies to both sides, so a bin that straddles a clipped light and a dark subject
// retains ownership of the dark edge. The full-resolution frame is changed in place so source
// preparation does not allocate a second full-size RGB raster.
[[nodiscard]] ClippedHighlightReconstructionStats reconstruct_clipped_highlight_surface(
    SceneLinearRgbFrame& scene_linear,
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk
);

} // namespace shadow::image::raw_pipeline_detail

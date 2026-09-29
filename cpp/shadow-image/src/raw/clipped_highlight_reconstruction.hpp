#pragma once

#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstdint>
#include <string_view>

namespace shadow::image {
struct RawFrameLinearTransform;
}

namespace shadow::image::raw_pipeline_detail {

inline constexpr std::string_view clipped_highlight_surface_reconstruction_identity =
    "clipped-highlight=boundary-propagated-scene-shoulder-v17";

// Continuous clipped-highlight treatment is the normal Shadow RAW source contract. The legacy
// aggressive value remains accepted as a wire/diagnostic alias; disabled is the only route that
// intentionally preserves the clipped projection's original chromaticity.
[[nodiscard]] constexpr bool
uses_clipped_highlight_surface_reconstruction(const RawHighlightRecoveryIntent intent) noexcept {
    return intent == RawHighlightRecoveryIntent::provider_default
           || intent == RawHighlightRecoveryIntent::aggressive;
}

// Ordinary Bayer development owns the same intent at the damaged CFA photosite. Keeping this
// predicate distinct from the camera-RGB surface route prevents a caller from disabling resident
// CFA execution merely because an AI/fallback source still needs post-demosaic reconstruction.
[[nodiscard]] constexpr bool
uses_cfa_owned_highlight_reconstruction(const RawHighlightRecoveryIntent intent) noexcept {
    return intent == RawHighlightRecoveryIntent::provider_default
           || intent == RawHighlightRecoveryIntent::aggressive;
}

struct ClippedHighlightReconstructionStats final {
    std::uint64_t clipped_pixel_count = 0U;
    std::uint64_t blended_pixel_count = 0U;
    Dimensions guide_dimensions;
};

// Marks CFA-stage highlight reconstruction as the owner of the source repair and converts the
// broad pre-reconstruction risk topology into a small residual confidence inside exact shared
// terminal coverage. Ordinary Bayer preview, detail, and export paths use this after their
// evidence-owned CFA reconstruction. The grade path may then suppress a remaining terminal tint
// during negative highlight recovery without reinterpreting the binary clipping boundary or
// expanding colour onto measured neighbours.
void complete_cfa_owned_highlight_reconstruction(
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk
);

// Softens source-unreliable highlight topology with bounded low-frequency local evidence at
// native/detail geometry. CFA-area previews instead complete their repair inside evidence-owned
// sampling and must not enter this post-demosaic RGB stage. A
// camera-space colour guide excludes clipped and CFA-risked samples while retaining their original
// confidence as fixed boundary seeds; inferred guide cells propagate only inside coherent clipping
// support and never become new seeds. A broader observed-light guide retains local luminance shape.
// Retained scene-referred WB headroom passes through a monotonic local shoulder inside the shared
// three-colour physical-clipping core. Single/two-channel clipping may repair unreliable chroma but
// cannot become a low-frequency luminance surface. Edge gates and the native-resolution risk map
// keep either repair from crossing onto an adjacent dark subject; measured pixels outside that
// explicit footprint remain unchanged. Source preparation then reduces the obsolete broad CFA-risk
// topology to bounded residual uncertainty inside the factual terminal core, so later recovery can
// remove a remaining tint without redrawing the source boundary. The full-resolution frame is
// changed in place and the extra propagation storage is bounded to the small guide.
[[nodiscard]] ClippedHighlightReconstructionStats reconstruct_clipped_highlight_surface(
    SceneLinearRgbFrame& scene_linear,
    const SensorClippingMask& sensor_clipping,
    HighlightChromaRiskMap& highlight_chroma_risk,
    const RawFrameLinearTransform* camera_transform = nullptr
);

} // namespace shadow::image::raw_pipeline_detail

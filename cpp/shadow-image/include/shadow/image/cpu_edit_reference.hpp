#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace shadow::image {

// Validates and deterministically repairs small spots from a smooth ring of
// surrounding pixels. This deliberately is not an inpainting/generative API:
// its result is fully determined by the current raster and stored targets.
void validate_spot_heal(const SpotHealAdjustment& adjustment);
void apply_spot_heal(
    FloatRgbImage& image,
    const SpotHealAdjustment& adjustment,
    AdjustmentExecutionContext context = {}
);

// Executes an intentionally compact subset of the future typed edit graph. The recommended
// default pipeline order is RgbWhiteBalance -> Exposure -> Contrast -> SelectiveTone ->
// Saturation -> PerceptualColor -> OklabLightnessToneCurve -> OklabOpponentToneCurves, but that is a recipe
// convention: this executor always applies nodes in the supplied span order.
// Disabled nodes are skipped and the input is never mutated. The executor does not clamp
// negative or >1 values and rejects NaN/Inf rather than silently contaminating caches.
[[nodiscard]] FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context = {}
);

// Applies a smooth curve to Oklab L only.  This standalone equivalent of an
// OklabLightnessToneCurve node is useful both for contract tests and future
// GPU parity tests; no output gamut clipping occurs here.
[[nodiscard]] FloatRgbImage apply_oklab_lightness_tone_curve(
    const FloatRgbImage& input,
    const OklabLightnessToneCurve& curve
);

// Samples the exact perceptual-curve evaluator at uniformly spaced x coordinates in [0, 1].
// UI code should draw these samples instead of fitting an unrelated display-only Bezier.
// sample_count must be between 2 and maximum_tone_curve_preview_samples, inclusive.
[[nodiscard]] std::vector<ToneCurvePoint> sample_smooth_tone_curve(
    const ToneCurveSet& curve,
    std::size_t sample_count
);

} // namespace shadow::image

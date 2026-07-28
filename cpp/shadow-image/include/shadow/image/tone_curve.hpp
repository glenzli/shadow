#pragma once

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <vector>

namespace shadow::image {

// Applies a smooth curve to Oklab L only. This standalone equivalent of an
// OklabLightnessToneCurve node is useful for contract tests and backend parity;
// no output gamut clipping occurs here.
[[nodiscard]] FloatRgbImage apply_oklab_lightness_tone_curve(const FloatRgbImage& input,
                                                             const OklabLightnessToneCurve& curve);

// Samples the exact perceptual-curve evaluator at uniformly spaced x coordinates in [0, 1].
// UI code should draw these samples instead of fitting an unrelated display-only Bezier.
// sample_count must be between 2 and maximum_tone_curve_preview_samples, inclusive.
[[nodiscard]] std::vector<ToneCurvePoint> sample_smooth_tone_curve(const ToneCurveSet& curve,
                                                                   std::size_t sample_count);

} // namespace shadow::image

#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/retouch.hpp>
#include <shadow/image/tone_curve.hpp>
#include <shadow/image/working_rgb.hpp>

#include <span>

namespace shadow::image {

// Executes an intentionally compact subset of the future typed edit graph. The recommended
// default pipeline order is RgbWhiteBalance -> Exposure -> Contrast -> SelectiveTone ->
// Saturation -> PerceptualColor -> OklabLightnessToneCurve -> OklabOpponentToneCurves, but that is
// a recipe convention: this executor always applies nodes in the supplied span order. Disabled
// nodes are skipped and the input is never mutated. The executor does not clamp negative or >1
// values and rejects NaN/Inf rather than silently contaminating caches.
[[nodiscard]] FloatRgbImage execute_adjustment_nodes(const FloatRgbImage& input,
                                                     std::span<const AdjustmentNode> nodes,
                                                     AdjustmentExecutionContext context = {});

} // namespace shadow::image

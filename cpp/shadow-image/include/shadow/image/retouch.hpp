#pragma once

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

namespace shadow::image {

// Validates and deterministically repairs authored spots or continuous brush strokes. This is
// deliberately not an inpainting/generative API: its result is fully determined by the current
// raster and stored retouch geometry.
void validate_spot_heal(const SpotHealAdjustment& adjustment);
void apply_spot_heal(
    FloatRgbImage& image,
    const SpotHealAdjustment& adjustment,
    AdjustmentExecutionContext context = {}
);

} // namespace shadow::image

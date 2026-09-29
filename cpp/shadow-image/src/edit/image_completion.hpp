#pragma once

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

namespace shadow::image::detail {

void validate_image_completion(const ImageCompletionAdjustment& adjustment);
void apply_image_completion(
    FloatRgbImage& image,
    const ImageCompletionAdjustment& adjustment,
    const AdjustmentExecutionContext& context
);

} // namespace shadow::image::detail

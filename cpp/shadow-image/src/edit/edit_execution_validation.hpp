#pragma once

namespace shadow::image {

struct AdjustmentExecutionContext;
struct FloatRgbImage;

namespace detail {

void validate_edit_image(const FloatRgbImage& image);

[[nodiscard]] AdjustmentExecutionContext
validate_edit_execution_context(const FloatRgbImage& input, AdjustmentExecutionContext context);

} // namespace detail

} // namespace shadow::image

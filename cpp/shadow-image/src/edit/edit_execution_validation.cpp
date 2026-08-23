#include "edit_execution_validation.hpp"

#include "working_color_math.hpp"

#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/sensor_clipping.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>

namespace shadow::image::detail {

namespace {

constexpr std::size_t rgb_channels = 3U;

} // namespace

void validate_edit_image(const FloatRgbImage& image) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input dimensions must be non-zero"
        );
    }
    if (image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input must use native interleaved RGB float32 samples"
        );
    }
    if (image.transfer_function != TransferFunction::linear
        || (image.reference != ImageReference::scene_referred
            && image.reference != ImageReference::display_referred)) {
        throw EditError(
            EditErrorCode::incompatible_color_encoding,
            std::nullopt,
            "edit input must be standardized linear RGB"
        );
    }
    if (!std::isfinite(image.level_zero_to_raster_scale_x)
        || image.level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(image.level_zero_to_raster_scale_y)
        || image.level_zero_to_raster_scale_y <= 0.0) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input level-0-to-raster scales must be finite and positive"
        );
    }

    const auto& space = image.working_space;
    if (space.id.empty() || !finite_chromaticity(space.white_point)
        || !std::ranges::all_of(space.primaries, finite_chromaticity)
        || !std::ranges::all_of(space.luminance_coefficients, [](const double value) {
               return std::isfinite(value);
           })) {
        throw EditError(
            EditErrorCode::invalid_working_space,
            std::nullopt,
            "edit input requires a finite, explicitly identified RGB working space"
        );
    }
    const double luminance_sum = space.luminance_coefficients[0] + space.luminance_coefficients[1]
                                 + space.luminance_coefficients[2];
    if (std::abs(luminance_sum - 1.0) > 1.0e-6) {
        throw EditError(
            EditErrorCode::invalid_working_space,
            std::nullopt,
            "working-space luminance coefficients must sum to one"
        );
    }

    const std::uint64_t minimum_row_samples =
        static_cast<std::uint64_t>(image.dimensions.width) * rgb_channels;
    if (minimum_row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input row size overflows the address space"
        );
    }
    const std::size_t minimum_stride =
        static_cast<std::size_t>(minimum_row_samples) * sizeof(float);
    if (image.row_stride_bytes < minimum_stride || image.row_stride_bytes % sizeof(float) != 0U) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input row stride is invalid"
        );
    }
    const std::size_t row_stride = image.row_stride_bytes / sizeof(float);
    const std::size_t height = static_cast<std::size_t>(image.dimensions.height);
    if (row_stride > std::numeric_limits<std::size_t>::max() / height) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input sample count overflows the address space"
        );
    }
    const std::size_t required_samples = row_stride * height;
    if (image.samples.size() != required_samples) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input sample storage must exactly match dimensions and stride"
        );
    }
    if (!std::ranges::all_of(image.samples, [](const float value) {
            return std::isfinite(value);
        })) {
        throw EditError(
            EditErrorCode::non_finite_value,
            std::nullopt,
            "edit input contains NaN or infinity"
        );
    }
}

AdjustmentExecutionContext
validate_edit_execution_context(const FloatRgbImage& input, AdjustmentExecutionContext context) {
    if (context.full_dimensions.width == 0U || context.full_dimensions.height == 0U) {
        context.full_dimensions = input.dimensions;
    }
    if (context.origin_x > context.full_dimensions.width
        || context.origin_y > context.full_dimensions.height
        || input.dimensions.width > context.full_dimensions.width - context.origin_x
        || input.dimensions.height > context.full_dimensions.height - context.origin_y) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "adjustment execution context lies outside its full raster"
        );
    }
    if (context.sensor_clipping_mask != nullptr
        && (!context.sensor_clipping_mask->valid()
            || context.sensor_clipping_mask->dimensions != context.full_dimensions)) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "adjustment execution context has incompatible sensor-clipping evidence"
        );
    }
    return context;
}

} // namespace shadow::image::detail

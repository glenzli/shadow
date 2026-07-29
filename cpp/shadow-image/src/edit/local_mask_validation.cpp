#include "local_mask_validation.hpp"

#include <shadow/image/edit_error.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

namespace {

[[noreturn]] void invalid_mask(std::string message) {
    throw EditError(EditErrorCode::invalid_parameter, std::nullopt, std::move(message));
}

void validate_normalized(const double value, const std::string_view name) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        invalid_mask("local-mask " + std::string(name) + " must be finite and in [0, 1]");
    }
}

[[nodiscard]] Dimensions validate_full_dimensions(
    const Dimensions input_dimensions,
    const AdjustmentExecutionContext context
) {
    const Dimensions full =
        context.full_dimensions.width == 0U || context.full_dimensions.height == 0U
            ? input_dimensions
            : context.full_dimensions;
    if (full.width == 0U || full.height == 0U || context.origin_x > full.width
        || context.origin_y > full.height || input_dimensions.width > full.width - context.origin_x
        || input_dimensions.height > full.height - context.origin_y) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "local-mask execution context exceeds its full-image dimensions"
        );
    }
    return full;
}

} // namespace

void validate_local_mask(const LocalMask& mask) {
    switch (mask.kind) {
    case LocalMaskKind::linear_gradient: {
        validate_normalized(mask.x0, "x0");
        validate_normalized(mask.y0, "y0");
        validate_normalized(mask.x1, "x1");
        validate_normalized(mask.y1, "y1");
        const double dx = mask.x1 - mask.x0;
        const double dy = mask.y1 - mask.y0;
        if (std::fma(dx, dx, dy * dy) <= std::numeric_limits<double>::epsilon()) {
            invalid_mask("local-mask linear gradient must have a non-zero direction");
        }
        return;
    }
    case LocalMaskKind::radial_gradient:
        validate_normalized(mask.x0, "x0");
        validate_normalized(mask.y0, "y0");
        validate_normalized(mask.radius_x, "radius x");
        validate_normalized(mask.radius_y, "radius y");
        validate_normalized(mask.feather, "feather");
        if (mask.radius_x <= 0.0 || mask.radius_y <= 0.0) {
            invalid_mask("local-mask radial gradient radii must both be greater than zero");
        }
        return;
    case LocalMaskKind::brush:
        validate_normalized(mask.radius_x, "brush radius");
        validate_normalized(mask.feather, "brush feather");
        if (mask.radius_x <= 0.0 || mask.points.size() > 4096U) {
            invalid_mask("local-mask brush radius or point count is invalid");
        }
        for (const auto& point : mask.points) {
            validate_normalized(point.x, "brush x");
            validate_normalized(point.y, "brush y");
        }
        return;
    case LocalMaskKind::luminance_range:
        validate_normalized(mask.x0, "luminance lower");
        validate_normalized(mask.x1, "luminance upper");
        validate_normalized(mask.feather, "luminance softness");
        if (mask.x0 > mask.x1) {
            invalid_mask("local-mask luminance lower bound must not exceed its upper bound");
        }
        return;
    case LocalMaskKind::color_range:
        validate_normalized(mask.x0, "color center hue");
        validate_normalized(mask.x1, "color half width");
        validate_normalized(mask.feather, "color softness");
        if (mask.x0 >= 1.0 || mask.x1 < 1.0 / 180.0) {
            invalid_mask(
                "local-mask color center must be canonical and half width must be in [1, 180] "
                "degrees"
            );
        }
        return;
    }
    invalid_mask("local-mask has an unsupported kind");
}

Dimensions validate_adjustment_layer_plan(
    const FloatRgbImage& input,
    const std::span<const AdjustmentLayer> layers,
    const AdjustmentExecutionContext context
) {
    return validate_adjustment_layer_plan(input.dimensions, layers, context);
}

Dimensions validate_adjustment_layer_plan(
    const Dimensions input_dimensions,
    const std::span<const AdjustmentLayer> layers,
    const AdjustmentExecutionContext context
) {
    if (layers.empty()) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "local-mask layer plan must contain at least one layer"
        );
    }
    const Dimensions full = validate_full_dimensions(input_dimensions, context);
    for (const AdjustmentLayer& layer : layers) {
        if (layer.layer_id.empty() || !std::isfinite(layer.opacity) || layer.opacity < 0.0
            || layer.opacity > 1.0 || layer.nodes.empty()) {
            invalid_mask("local-mask layer has invalid identity, opacity, or node content");
        }
        if (layer.mask.has_value()) {
            validate_local_mask(*layer.mask);
        }
        // Bypassing a layer must not turn malformed persisted node content into an executable
        // Recipe later.
        validate_adjustment_nodes(layer.nodes);
    }
    return full;
}

} // namespace shadow::image::detail

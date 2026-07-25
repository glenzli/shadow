#include <shadow/image/edit.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace shadow::image {

namespace {

[[noreturn]] void invalid_mask(std::string message) {
    throw EditError(EditErrorCode::invalid_parameter, std::nullopt, std::move(message));
}

void validate_normalized(const double value, const std::string_view name) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        invalid_mask("local-mask " + std::string(name) + " must be finite and in [0, 1]");
    }
}

void validate_mask(const LocalMask& mask) {
    validate_normalized(mask.x0, "x0");
    validate_normalized(mask.y0, "y0");
    switch (mask.kind) {
    case LocalMaskKind::linear_gradient: {
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
        validate_normalized(mask.radius_x, "radius x");
        validate_normalized(mask.radius_y, "radius y");
        validate_normalized(mask.feather, "feather");
        if (mask.radius_x <= 0.0 || mask.radius_y <= 0.0) {
            invalid_mask("local-mask radial gradient radii must both be greater than zero");
        }
        return;
    }
    invalid_mask("local-mask has an unsupported kind");
}

[[nodiscard]] double smootherstep(const double value) noexcept {
    const double x = std::clamp(value, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double coverage_at(
    const LocalMask& mask,
    const double x,
    const double y
) noexcept {
    double coverage = 0.0;
    switch (mask.kind) {
    case LocalMaskKind::linear_gradient: {
        const double dx = mask.x1 - mask.x0;
        const double dy = mask.y1 - mask.y0;
        const double denominator = std::fma(dx, dx, dy * dy);
        coverage = std::clamp(((x - mask.x0) * dx + (y - mask.y0) * dy) / denominator, 0.0, 1.0);
        break;
    }
    case LocalMaskKind::radial_gradient: {
        const double dx = (x - mask.x0) / mask.radius_x;
        const double dy = (y - mask.y0) / mask.radius_y;
        const double distance = std::sqrt(std::fma(dx, dx, dy * dy));
        if (mask.feather <= 0.0) {
            coverage = distance <= 1.0 ? 1.0 : 0.0;
        } else {
            const double inner = 1.0 - mask.feather;
            coverage = 1.0 - smootherstep((distance - inner) / mask.feather);
        }
        break;
    }
    }
    return mask.invert ? 1.0 - coverage : coverage;
}

[[nodiscard]] Dimensions full_dimensions(
    const FloatRgbImage& input,
    const AdjustmentExecutionContext context
) {
    const Dimensions full = context.full_dimensions.width == 0U
            || context.full_dimensions.height == 0U
        ? input.dimensions
        : context.full_dimensions;
    if (full.width == 0U || full.height == 0U
        || context.origin_x > full.width || context.origin_y > full.height
        || input.dimensions.width > full.width - context.origin_x
        || input.dimensions.height > full.height - context.origin_y) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "local-mask execution context exceeds its full-image dimensions"
        );
    }
    return full;
}

void mix_masked_layer(
    FloatRgbImage& destination,
    const FloatRgbImage& source,
    const LocalMask& mask,
    const double opacity,
    const AdjustmentExecutionContext context,
    const Dimensions full
) {
    const std::size_t stride = destination.row_stride_bytes / sizeof(float);
    const auto width = static_cast<std::size_t>(destination.dimensions.width);
    const auto height = static_cast<std::size_t>(destination.dimensions.height);
    for (std::size_t row = 0U; row < height; ++row) {
        const double y = (static_cast<double>(context.origin_y) + static_cast<double>(row) + 0.5)
            / static_cast<double>(full.height);
        for (std::size_t column = 0U; column < width; ++column) {
            const double x = (static_cast<double>(context.origin_x)
                                  + static_cast<double>(column) + 0.5)
                / static_cast<double>(full.width);
            const double alpha = opacity * coverage_at(mask, x, y);
            const std::size_t sample = row * stride + column * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const double before = static_cast<double>(source.samples[sample + channel]);
                const double after = static_cast<double>(destination.samples[sample + channel]);
                const double mixed = std::fma(alpha, after - before, before);
                if (!std::isfinite(mixed)
                    || mixed < static_cast<double>(std::numeric_limits<float>::lowest())
                    || mixed > static_cast<double>(std::numeric_limits<float>::max())) {
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "local-mask layer blend produced an invalid RGB sample"
                    );
                }
                destination.samples[sample + channel] = static_cast<float>(mixed);
            }
        }
    }
}

} // namespace

FloatRgbImage execute_adjustment_layers(
    const FloatRgbImage& input,
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
    const Dimensions full = full_dimensions(input, context);
    FloatRgbImage output = input;
    for (const auto& layer : layers) {
        if (layer.layer_id.empty() || !std::isfinite(layer.opacity)
            || layer.opacity < 0.0 || layer.opacity > 1.0 || layer.nodes.empty()) {
            invalid_mask("local-mask layer has invalid identity, opacity, or node content");
        }
        if (layer.mask.has_value()) {
            validate_mask(*layer.mask);
        }
        // Validating all enclosed nodes even for a bypassed layer keeps a malformed
        // persisted Recipe from becoming executable merely because it is currently off.
        validate_adjustment_nodes(layer.nodes);
        if (!layer.enabled || layer.opacity == 0.0) {
            continue;
        }
        if (!layer.mask.has_value() && layer.opacity == 1.0) {
            output = execute_adjustment_nodes(output, layer.nodes, context);
            continue;
        }
        const FloatRgbImage source = output;
        output = execute_adjustment_nodes(output, layer.nodes, context);
        if (layer.mask.has_value()) {
            mix_masked_layer(output, source, *layer.mask, layer.opacity, context, full);
        } else {
            // Opacity without an explicit spatial mask is a whole-image blend.
            // A synthetic all-covered spatial mask would add needless per-pixel
            // coordinate work, so this path blends directly.
            const std::size_t count = output.samples.size();
            for (std::size_t index = 0U; index < count; ++index) {
                const double before = static_cast<double>(source.samples[index]);
                const double after = static_cast<double>(output.samples[index]);
                const double mixed = std::fma(layer.opacity, after - before, before);
                if (!std::isfinite(mixed)
                    || mixed < static_cast<double>(std::numeric_limits<float>::lowest())
                    || mixed > static_cast<double>(std::numeric_limits<float>::max())) {
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "local-mask layer opacity blend produced an invalid RGB sample"
                    );
                }
                output.samples[index] = static_cast<float>(mixed);
            }
        }
    }
    return output;
}

} // namespace shadow::image

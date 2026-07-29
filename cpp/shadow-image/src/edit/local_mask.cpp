#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include "local_mask_validation.hpp"
#include "perceptual_hue_selection.hpp"
#include "working_color_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace shadow::image {

namespace {

[[nodiscard]] double smootherstep(const double value) noexcept {
    const double x = std::clamp(value, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double segment_distance(
    const double x,
    const double y,
    const LocalMaskPoint& start,
    const LocalMaskPoint& end,
    const double scale_x,
    const double scale_y
) noexcept {
    const double scaled_x = x * scale_x;
    const double scaled_y = y * scale_y;
    const double start_x = start.x * scale_x;
    const double start_y = start.y * scale_y;
    const double end_x = end.x * scale_x;
    const double end_y = end.y * scale_y;
    const double dx = end_x - start_x;
    const double dy = end_y - start_y;
    const double denominator = std::fma(dx, dx, dy * dy);
    if (denominator <= std::numeric_limits<double>::epsilon()) {
        return std::hypot(scaled_x - end_x, scaled_y - end_y);
    }
    const double t =
        std::clamp(((scaled_x - start_x) * dx + (scaled_y - start_y) * dy) / denominator, 0.0, 1.0);
    return std::hypot(scaled_x - std::fma(t, dx, start_x), scaled_y - std::fma(t, dy, start_y));
}

struct PreparedLocalMask final {
    const LocalMask& mask;
    double brush_scale_x = 1.0;
    double brush_scale_y = 1.0;
    std::optional<detail::WorkingSpaceTransform> color_transform;
};

[[nodiscard]] PreparedLocalMask
prepare_local_mask(const LocalMask& mask, const FloatRgbImage& source, const Dimensions full) {
    const double shorter_side = static_cast<double>(std::min(full.width, full.height));
    PreparedLocalMask prepared{
        .mask = mask,
        .brush_scale_x = static_cast<double>(full.width) / shorter_side,
        .brush_scale_y = static_cast<double>(full.height) / shorter_side,
    };
    if (mask.kind == LocalMaskKind::luminance_range || mask.kind == LocalMaskKind::color_range) {
        prepared.color_transform = detail::prepare_working_space_transform(source.working_space);
    }
    return prepared;
}

[[nodiscard]] double
luminance_range_coverage(const LocalMask& mask, const double lightness) noexcept {
    const double selected_lightness = std::clamp(lightness, 0.0, 1.0);
    if (mask.feather <= 0.0) {
        return selected_lightness >= mask.x0 && selected_lightness <= mask.x1 ? 1.0 : 0.0;
    }
    const double lower =
        smootherstep((selected_lightness - (mask.x0 - mask.feather)) / mask.feather);
    const double upper = 1.0 - smootherstep((selected_lightness - mask.x1) / mask.feather);
    return std::min(lower, upper);
}

[[nodiscard]] double coverage_at(
    const PreparedLocalMask& prepared,
    const double x,
    const double y,
    const detail::Vector3& source_rgb
) noexcept {
    const LocalMask& mask = prepared.mask;
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
    case LocalMaskKind::brush: {
        if (mask.points.empty()) {
            coverage = 0.0;
            break;
        }
        double distance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0U; index < mask.points.size(); ++index) {
            const auto& point = mask.points[index];
            distance = std::min(
                distance,
                std::hypot(
                    (x - point.x) * prepared.brush_scale_x,
                    (y - point.y) * prepared.brush_scale_y
                )
            );
            if (index > 0U && !point.begins_stroke) {
                distance = std::min(
                    distance,
                    segment_distance(
                        x,
                        y,
                        mask.points[index - 1U],
                        point,
                        prepared.brush_scale_x,
                        prepared.brush_scale_y
                    )
                );
            }
        }
        const double inner = mask.radius_x * (1.0 - mask.feather);
        const double transition =
            std::max(mask.radius_x - inner, std::numeric_limits<double>::epsilon());
        coverage = mask.feather <= 0.0 ? (distance <= mask.radius_x ? 1.0 : 0.0)
                                       : 1.0 - smootherstep((distance - inner) / transition);
        break;
    }
    case LocalMaskKind::luminance_range: {
        const detail::Vector3 lab =
            detail::working_rgb_to_oklab(*prepared.color_transform, source_rgb);
        coverage = luminance_range_coverage(mask, lab[0]);
        break;
    }
    case LocalMaskKind::color_range: {
        const detail::Vector3 lab =
            detail::working_rgb_to_oklab(*prepared.color_transform, source_rgb);
        const detail::PerceptualHueSample hue = detail::sample_oklab_hue(lab);
        coverage = hue.confidence
                   * detail::perceptual_hue_range_weight(
                       hue.degrees,
                       mask.x0 * 360.0,
                       mask.x1 * 180.0,
                       mask.feather
                   );
        break;
    }
    }
    return mask.invert ? 1.0 - coverage : coverage;
}

void mix_masked_layer(
    FloatRgbImage& destination,
    const FloatRgbImage& source,
    const LocalMask& mask,
    const double opacity,
    const AdjustmentExecutionContext context,
    const Dimensions full
) {
    const PreparedLocalMask prepared = prepare_local_mask(mask, source, full);
    const std::size_t stride = destination.row_stride_bytes / sizeof(float);
    const auto width = static_cast<std::size_t>(destination.dimensions.width);
    const auto height = static_cast<std::size_t>(destination.dimensions.height);
    for (std::size_t row = 0U; row < height; ++row) {
        const double y = (static_cast<double>(context.origin_y) + static_cast<double>(row) + 0.5)
                         / static_cast<double>(full.height);
        for (std::size_t column = 0U; column < width; ++column) {
            const double x =
                (static_cast<double>(context.origin_x) + static_cast<double>(column) + 0.5)
                / static_cast<double>(full.width);
            const std::size_t sample = row * stride + column * 3U;
            const detail::Vector3 source_rgb{
                static_cast<double>(source.samples[sample]),
                static_cast<double>(source.samples[sample + 1U]),
                static_cast<double>(source.samples[sample + 2U]),
            };
            const double alpha = opacity * coverage_at(prepared, x, y, source_rgb);
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
    const Dimensions full = detail::validate_adjustment_layer_plan(input, layers, context);
    FloatRgbImage output = input;
    for (const auto& layer : layers) {
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

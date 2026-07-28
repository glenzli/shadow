#include "perceptual_contrast.hpp"

#include "adjustment_node_diagnostics.hpp"
#include "rgb_pixel_traversal.hpp"
#include "working_color_math.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <cmath>

namespace shadow::image::detail {

PreparedPerceptualContrast::PreparedPerceptualContrast(
    const bool neutral, const double pivot_lightness, const double signed_amount,
    const bool collapses_to_pivot
) noexcept
    : neutral_(neutral), pivot_lightness_(pivot_lightness), signed_amount_(signed_amount),
      collapses_to_pivot_(collapses_to_pivot) {}

PreparedPerceptualContrast prepare_perceptual_contrast(
    const ContrastAdjustment& parameters,
    const AdjustmentNode& node,
    const std::size_t node_index
) {
    if (
        !std::isfinite(parameters.factor) || parameters.factor < 0.0
        || !std::isfinite(parameters.pivot) || parameters.pivot < 0.0
    ) {
        throw_node_error(
            EditErrorCode::invalid_parameter,
            node_index,
            node,
            "contrast factor and pivot must be finite and non-negative"
        );
    }

    const bool neutral = parameters.factor == 1.0;
    const bool collapses_to_pivot = parameters.factor == 0.0;
    const double pivot_lightness = std::cbrt(std::max(parameters.pivot, 1.0e-9));
    const double signed_amount = collapses_to_pivot
        ? 0.0
        : std::clamp(std::log2(parameters.factor) * 0.20, -0.45, 0.45);
    return PreparedPerceptualContrast{
        neutral,
        pivot_lightness,
        signed_amount,
        collapses_to_pivot,
    };
}

namespace {

// Map Oklab lightness through a bounded contrast curve while preserving the a/b chroma axes.
// This keeps contrast perceptually consistent across hues and working RGB primaries. It remains
// a global per-pixel operation, so a detail tile and a full preview produce the same result.
[[nodiscard]] Vector3 apply_perceptual_contrast(
    const Vector3& input,
    const WorkingSpaceTransform& color_transform,
    const PreparedPerceptualContrast& prepared
) noexcept {
    if (prepared.neutral()) {
        return input;
    }

    Vector3 lab = working_rgb_to_oklab(color_transform, input);
    if (!(lab[0] > 0.0) || !std::isfinite(lab[0])) {
        return input;
    }

    if (prepared.collapses_to_pivot()) {
        lab[0] = prepared.pivot_lightness();
        return oklab_to_working_rgb(color_transform, lab);
    }

    const double normalized = lab[0] / (lab[0] + prepared.pivot_lightness());
    const double shaped = normalized
        + prepared.signed_amount() * 2.0 * normalized * (1.0 - normalized)
            * (2.0 * normalized - 1.0);
    const double bounded = std::clamp(shaped, 1.0e-7, 1.0 - 1.0e-7);
    lab[0] = prepared.pivot_lightness() * bounded / (1.0 - bounded);
    return oklab_to_working_rgb(color_transform, lab);
}

} // namespace

void apply_prepared_perceptual_contrast_cpu(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const PreparedPerceptualContrast& prepared
) {
    if (prepared.neutral()) {
        return;
    }
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    transform_rgb_pixels(
        image,
        node_index,
        node,
        [&prepared, &color_transform](const Vector3& input) {
            return apply_perceptual_contrast(input, color_transform, prepared);
        }
    );
}

} // namespace shadow::image::detail

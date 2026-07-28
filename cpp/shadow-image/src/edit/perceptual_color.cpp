#include "perceptual_color.hpp"

#include "adjustment_node_diagnostics.hpp"
#include "rgb_pixel_traversal.hpp"
#include "working_color_math.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>

namespace shadow::image::detail {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double perceptual_low_chroma_ratio_epsilon = 1.0e-7;

// Public color-mixer order: red, orange, yellow, green, cyan, blue, purple,
// magenta. These Oklch angles are derived from the named linear-sRGB anchors
// in the same D65 Oklab transform as the CPU and Metal implementations.
constexpr std::array<double, perceptual_hue_band_count> perceptual_hue_anchors{
    29.23388536933038,  52.98468002449970,  109.76923279602303, 142.49533925535556,
    194.76894786887132, 264.05202307198110, 293.93764240298924, 328.36341829329797,
};

// Photoshop-style Selective Color has six chromatic target families. Orange
// and purple intentionally blend between their adjacent primary families.
constexpr std::array<double, 6U> selective_color_hue_anchors{
    29.23388536933038,  109.76923279602303, 142.49533925535556,
    194.76894786887132, 264.05202307198110, 328.36341829329797,
};

[[nodiscard]] double smooth_transition(const double lower, const double upper,
                                       const double value) noexcept {
    if (value <= lower) {
        return 0.0;
    }
    if (value >= upper) {
        return 1.0;
    }
    const double normalized = (value - lower) / (upper - lower);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

[[nodiscard]] double wrap_degrees(const double degrees) noexcept {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

[[nodiscard]] double signed_hue_distance(const double hue, const double center) noexcept {
    return std::remainder(hue - center, 360.0);
}

[[nodiscard]] std::array<double, perceptual_hue_band_count>
hue_band_weights(const double hue) noexcept {
    std::array<double, perceptual_hue_band_count> weights{};
    const double wrapped_hue = wrap_degrees(hue);
    const auto upper =
        std::upper_bound(perceptual_hue_anchors.begin(), perceptual_hue_anchors.end(), wrapped_hue);
    const std::size_t right =
        upper == perceptual_hue_anchors.end()
            ? 0U
            : static_cast<std::size_t>(upper - perceptual_hue_anchors.begin());
    const std::size_t left = right == 0U ? weights.size() - 1U : right - 1U;
    const double left_hue = perceptual_hue_anchors[left];
    const double right_hue =
        right == 0U ? perceptual_hue_anchors.front() + 360.0 : perceptual_hue_anchors[right];
    const double unwrapped_hue =
        right == 0U && wrapped_hue < left_hue ? wrapped_hue + 360.0 : wrapped_hue;
    const double position =
        std::clamp((unwrapped_hue - left_hue) / (right_hue - left_hue), 0.0, 1.0);
    const double right_weight = 0.5 * (1.0 - std::cos(pi * position));
    weights[left] = 1.0 - right_weight;
    weights[right] = right_weight;
    if (left == right) {
        weights[left] = 1.0;
    }
    return weights;
}

[[nodiscard]] std::array<double, 6U> selective_color_hue_weights(const double hue) noexcept {
    std::array<double, 6U> weights{};
    const double wrapped_hue = wrap_degrees(hue);
    const auto upper = std::upper_bound(selective_color_hue_anchors.begin(),
                                        selective_color_hue_anchors.end(), wrapped_hue);
    const std::size_t right =
        upper == selective_color_hue_anchors.end()
            ? 0U
            : static_cast<std::size_t>(upper - selective_color_hue_anchors.begin());
    const std::size_t left = right == 0U ? weights.size() - 1U : right - 1U;
    const double left_hue = selective_color_hue_anchors[left];
    const double right_hue = right == 0U ? selective_color_hue_anchors.front() + 360.0
                                         : selective_color_hue_anchors[right];
    const double unwrapped_hue =
        right == 0U && wrapped_hue < left_hue ? wrapped_hue + 360.0 : wrapped_hue;
    const double position =
        std::clamp((unwrapped_hue - left_hue) / (right_hue - left_hue), 0.0, 1.0);
    const double right_weight = 0.5 * (1.0 - std::cos(pi * position));
    weights[left] = 1.0 - right_weight;
    weights[right] = right_weight;
    return weights;
}

[[nodiscard]] double color_range_weight(const PerceptualColorRange& range,
                                        const double hue) noexcept {
    if (!range.enabled) {
        return 0.0;
    }
    const double distance = std::abs(signed_hue_distance(hue, range.center_degrees));
    const double feather = range.width_degrees * range.softness;
    if (feather == 0.0) {
        return distance <= range.width_degrees ? 1.0 : 0.0;
    }
    const double fully_selected = range.width_degrees - feather;
    return 1.0 - smooth_transition(fully_selected, range.width_degrees, distance);
}

[[nodiscard]] bool apply_ordered_color_range(Vector3& lab,
                                             const PerceptualColorRange& range) noexcept {
    if (!range.enabled) {
        return false;
    }
    const double chroma = std::hypot(lab[1], lab[2]);
    const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
    const double confidence = smooth_transition(0.002, 0.02, relative_chroma);
    if (confidence == 0.0) {
        return false;
    }
    const double hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
    const double weight = confidence * color_range_weight(range, hue);
    if (weight == 0.0) {
        return false;
    }
    const double adjusted_hue = (hue + weight * range.hue_shift_degrees) * pi / 180.0;
    const double adjusted_chroma = chroma * (1.0 + weight * range.saturation);
    lab[0] += 0.15 * weight * range.lightness;
    lab[1] = adjusted_chroma * std::cos(adjusted_hue);
    lab[2] = adjusted_chroma * std::sin(adjusted_hue);
    return range.hue_shift_degrees != 0.0 || range.saturation != 0.0 || range.lightness != 0.0;
}

template <std::size_t Size>
[[nodiscard]] double weighted_sum(const std::array<double, Size>& values,
                                  const std::array<double, Size>& weights) noexcept {
    double result = 0.0;
    for (std::size_t index = 0U; index < Size; ++index) {
        result += values[index] * weights[index];
    }
    return result;
}

[[nodiscard]] bool normalized_amount(const double value) noexcept {
    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

[[nodiscard]] bool
perceptual_hue_mapping_is_neutral(const PerceptualColorAdjustment& parameters) noexcept {
    const bool bands_are_neutral =
        std::ranges::all_of(parameters.hue, [](const double value) { return value == 0.0; }) &&
        std::ranges::all_of(parameters.saturation,
                            [](const double value) { return value == 0.0; }) &&
        std::ranges::all_of(parameters.lightness, [](const double value) { return value == 0.0; });
    const auto range_is_neutral = [](const PerceptualColorRange& range) {
        return !range.enabled || (range.hue_shift_degrees == 0.0 && range.saturation == 0.0 &&
                                  range.lightness == 0.0);
    };
    const bool ranges_are_neutral =
        range_is_neutral(parameters.color_range) &&
        std::ranges::all_of(parameters.additional_color_ranges, range_is_neutral);
    return parameters.vibrance == 0.0 && bands_are_neutral && ranges_are_neutral;
}

[[nodiscard]] bool
selective_color_is_neutral(const PerceptualColorAdjustment& parameters) noexcept {
    return std::ranges::all_of(parameters.selective_color_cmyk, [](const auto& target) {
        return std::ranges::all_of(target, [](const double value) { return value == 0.0; });
    });
}

[[nodiscard]] bool
apply_global_oklab_opponent_balance(Vector3& lab,
                                    const PerceptualColorAdjustment& parameters) noexcept {
    if (parameters.global_a_balance == 0.0 && parameters.global_b_balance == 0.0) {
        return false;
    }
    constexpr double maximum_axis_offset = 0.075;
    const double low_light_protection = smooth_transition(0.015, 0.090, std::max(0.0, lab[0]));
    lab[1] += parameters.global_a_balance * maximum_axis_offset * low_light_protection;
    lab[2] += parameters.global_b_balance * maximum_axis_offset * low_light_protection;
    return true;
}

[[nodiscard]] std::array<double, selective_color_target_count>
selective_color_target_weights(const Vector3& lab) noexcept {
    std::array<double, selective_color_target_count> weights{};
    const double lightness = std::clamp(lab[0], 0.0, 1.0);
    const double chroma = std::hypot(lab[1], lab[2]);
    const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
    const double chromatic = smooth_transition(0.002, 0.08, relative_chroma);
    if (chromatic > 0.0) {
        const double hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
        const auto hue_weights = selective_color_hue_weights(hue);
        for (std::size_t index = 0U; index < hue_weights.size(); ++index) {
            weights[index] = chromatic * hue_weights[index];
        }
    }
    const double neutral = 1.0 - chromatic;
    weights[6] = neutral * smooth_transition(0.62, 0.94, lightness);
    weights[8] = neutral * (1.0 - smooth_transition(0.06, 0.38, lightness));
    weights[7] = std::max(0.0, neutral - weights[6] - weights[8]);
    return weights;
}

[[nodiscard]] Vector3 apply_selective_color(const Vector3& input,
                                            const PerceptualColorAdjustment& parameters,
                                            const WorkingSpaceTransform& color_transform) noexcept {
    const Vector3 lab = working_rgb_to_oklab(color_transform, input);
    const auto target_weights = selective_color_target_weights(lab);
    std::array<double, selective_color_component_count> adjustment{};
    for (std::size_t target = 0U; target < selective_color_target_count; ++target) {
        for (std::size_t component = 0U; component < selective_color_component_count; ++component) {
            adjustment[component] +=
                target_weights[target] * parameters.selective_color_cmyk[target][component];
        }
    }
    if (std::ranges::all_of(adjustment, [](const double value) { return value == 0.0; })) {
        return input;
    }

    const double peak = std::max({1.0, input[0], input[1], input[2]});
    const double red = std::clamp(input[0] / peak, 0.0, 1.0);
    const double green = std::clamp(input[1] / peak, 0.0, 1.0);
    const double blue = std::clamp(input[2] / peak, 0.0, 1.0);
    const double key = 1.0 - std::max({red, green, blue});
    const double chromatic_denominator = 1.0 - key;
    std::array<double, selective_color_component_count> cmyk{
        chromatic_denominator > 1.0e-9 ? (1.0 - red - key) / chromatic_denominator : 0.0,
        chromatic_denominator > 1.0e-9 ? (1.0 - green - key) / chromatic_denominator : 0.0,
        chromatic_denominator > 1.0e-9 ? (1.0 - blue - key) / chromatic_denominator : 0.0,
        key,
    };
    for (std::size_t component = 0U; component < selective_color_component_count; ++component) {
        const double amount = parameters.selective_color_relative
                                  ? cmyk[component] * adjustment[component]
                                  : adjustment[component];
        cmyk[component] = std::clamp(cmyk[component] + amount, 0.0, 1.0);
    }
    const double ink_scale = 1.0 - cmyk[3];
    Vector3 output{
        peak * (1.0 - cmyk[0]) * ink_scale,
        peak * (1.0 - cmyk[1]) * ink_scale,
        peak * (1.0 - cmyk[2]) * ink_scale,
    };
    if (parameters.selective_color_lightness_protection > 0.0) {
        Vector3 corrected_lab = working_rgb_to_oklab(color_transform, output);
        corrected_lab[0] =
            std::lerp(corrected_lab[0], lab[0], parameters.selective_color_lightness_protection);
        output = oklab_to_working_rgb(color_transform, corrected_lab);
    }
    return output;
}

} // namespace

PerceptualColorStages
classify_perceptual_color(const PerceptualColorAdjustment& parameters) noexcept {
    return PerceptualColorStages{
        !perceptual_hue_mapping_is_neutral(parameters),
        parameters.global_a_balance != 0.0 || parameters.global_b_balance != 0.0,
        !selective_color_is_neutral(parameters),
    };
}

std::span<const double> perceptual_color_hue_anchors() noexcept { return perceptual_hue_anchors; }

void validate_perceptual_color(const PerceptualColorAdjustment& parameters,
                               const AdjustmentNode& node, const std::size_t node_index) {
    const bool valid_bands = std::ranges::all_of(parameters.hue, normalized_amount) &&
                             std::ranges::all_of(parameters.saturation, normalized_amount) &&
                             std::ranges::all_of(parameters.lightness, normalized_amount);
    const bool valid_selective_color =
        std::ranges::all_of(parameters.selective_color_cmyk, [](const auto& target) {
            return std::ranges::all_of(target, normalized_amount);
        });
    const auto valid_range = [](const PerceptualColorRange& range) {
        return std::isfinite(range.center_degrees) && range.center_degrees >= 0.0 &&
               range.center_degrees <= 360.0 && std::isfinite(range.width_degrees) &&
               range.width_degrees >= 1.0 && range.width_degrees <= 180.0 &&
               std::isfinite(range.softness) && range.softness >= 0.0 && range.softness <= 1.0 &&
               std::isfinite(range.hue_shift_degrees) && range.hue_shift_degrees >= -180.0 &&
               range.hue_shift_degrees <= 180.0 && normalized_amount(range.saturation) &&
               normalized_amount(range.lightness);
    };
    const bool valid_ranges =
        valid_range(parameters.color_range) &&
        parameters.additional_color_ranges.size() + 1U <= maximum_point_color_ranges &&
        std::ranges::all_of(parameters.additional_color_ranges, valid_range);
    if (!normalized_amount(parameters.global_a_balance) ||
        !normalized_amount(parameters.global_b_balance) ||
        !normalized_amount(parameters.vibrance) || !valid_bands || !valid_ranges ||
        !valid_selective_color || !std::isfinite(parameters.selective_color_lightness_protection) ||
        parameters.selective_color_lightness_protection < 0.0 ||
        parameters.selective_color_lightness_protection > 1.0) {
        throw_node_error(EditErrorCode::invalid_parameter, node_index, node,
                         "perceptual color parameters are outside their finite declared bounds");
    }
}

namespace {

[[nodiscard]] Vector3 apply_perceptual_color_pixel(const Vector3& input,
                                                   const PerceptualColorAdjustment& parameters,
                                                   const WorkingSpaceTransform& color_transform,
                                                   const PerceptualColorStages stages) noexcept {
    if (stages.neutral()) {
        return input;
    }
    if (!stages.oklab_pipeline_active()) {
        return apply_selective_color(input, parameters, color_transform);
    }

    Vector3 lab = working_rgb_to_oklab(color_transform, input);
    Vector3 adjusted = input;
    bool changed = false;
    if (stages.hue_mapping_active()) {
        const double chroma = std::hypot(lab[1], lab[2]);
        const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
        if (relative_chroma > perceptual_low_chroma_ratio_epsilon) {
            const double source_hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
            const auto band_weights = hue_band_weights(source_hue);
            const double hue_confidence = smooth_transition(0.002, 0.02, relative_chroma);
            const double band_hue = hue_confidence * weighted_sum(parameters.hue, band_weights);
            const double band_saturation =
                weighted_sum(parameters.saturation, band_weights) * hue_confidence;
            const double band_lightness =
                hue_confidence * weighted_sum(parameters.lightness, band_weights);
            const double range_weight =
                hue_confidence * color_range_weight(parameters.color_range, source_hue);
            const double vibrance_weight = 1.0 - smooth_transition(0.05, 0.35, relative_chroma);
            const double chroma_factor = (1.0 + parameters.vibrance * vibrance_weight) *
                                         (1.0 + band_saturation) *
                                         (1.0 + range_weight * parameters.color_range.saturation);
            const double hue_delta =
                30.0 * band_hue + range_weight * parameters.color_range.hue_shift_degrees;
            const double lightness_delta =
                0.15 * (band_lightness + range_weight * parameters.color_range.lightness);
            changed = chroma_factor != 1.0 || hue_delta != 0.0 || lightness_delta != 0.0;
            if (changed) {
                const double adjusted_hue = (source_hue + hue_delta) * pi / 180.0;
                const double adjusted_chroma = chroma * chroma_factor;
                lab[0] += lightness_delta;
                lab[1] = adjusted_chroma * std::cos(adjusted_hue);
                lab[2] = adjusted_chroma * std::sin(adjusted_hue);
            }
            for (const auto& range : parameters.additional_color_ranges) {
                changed = apply_ordered_color_range(lab, range) || changed;
            }
        }
    }
    if (stages.opponent_balance_active()) {
        changed = apply_global_oklab_opponent_balance(lab, parameters) || changed;
    }
    if (changed) {
        adjusted = oklab_to_working_rgb(color_transform, lab);
    }
    return stages.selective_color_active()
               ? apply_selective_color(adjusted, parameters, color_transform)
               : adjusted;
}

} // namespace

void apply_perceptual_color_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                const std::size_t node_index,
                                const PerceptualColorAdjustment& parameters,
                                const PerceptualColorStages stages) {
    if (stages.neutral()) {
        return;
    }
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    transform_rgb_pixels(
        image, node_index, node, [&parameters, &color_transform, stages](const Vector3& input) {
            return apply_perceptual_color_pixel(input, parameters, color_transform, stages);
        });
}

} // namespace shadow::image::detail

// Included once inside cpu_reference.cpp's private namespace.
// Perceptual hue/chroma operations and Selective Color form the creative
// color stage while preserving the executor's exact working-space transform.

[[nodiscard]] double wrap_degrees(const double degrees) noexcept {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

[[nodiscard]] double signed_hue_distance(
    const double hue,
    const double center
) noexcept {
    return std::remainder(hue - center, 360.0);
}

[[nodiscard]] std::array<double, perceptual_hue_band_count> hue_band_weights(
    const double hue
) noexcept {
    std::array<double, perceptual_hue_band_count> weights{};

    const double wrapped_hue = wrap_degrees(hue);
    const auto upper = std::upper_bound(
        perceptual_hue_anchors.begin(),
        perceptual_hue_anchors.end(),
        wrapped_hue
    );
    const std::size_t right = upper == perceptual_hue_anchors.end()
        ? 0U
        : static_cast<std::size_t>(upper - perceptual_hue_anchors.begin());
    const std::size_t left = right == 0U ? weights.size() - 1U : right - 1U;

    const double left_hue = perceptual_hue_anchors[left];
    const double right_hue = right == 0U
        ? perceptual_hue_anchors.front() + 360.0
        : perceptual_hue_anchors[right];
    const double unwrapped_hue = right == 0U && wrapped_hue < left_hue
        ? wrapped_hue + 360.0
        : wrapped_hue;
    const double position = std::clamp(
        (unwrapped_hue - left_hue) / (right_hue - left_hue),
        0.0,
        1.0
    );

    // A raised-cosine crossfade is C1 at every non-uniform anchor. Assigning
    // the left weight as the complement makes the periodic pair an exact
    // partition of unity (including the magenta/red seam).
    const double right_weight = 0.5 * (1.0 - std::cos(pi * position));
    weights[left] = 1.0 - right_weight;
    weights[right] = right_weight;
    if (left == right) {
        weights[left] = 1.0;
    }
    return weights;
}

[[nodiscard]] std::array<double, 6U> selective_color_hue_weights(
    const double hue
) noexcept {
    std::array<double, 6U> weights{};
    const double wrapped_hue = wrap_degrees(hue);
    const auto upper = std::upper_bound(
        selective_color_hue_anchors.begin(),
        selective_color_hue_anchors.end(),
        wrapped_hue
    );
    const std::size_t right = upper == selective_color_hue_anchors.end()
        ? 0U
        : static_cast<std::size_t>(upper - selective_color_hue_anchors.begin());
    const std::size_t left = right == 0U ? weights.size() - 1U : right - 1U;
    const double left_hue = selective_color_hue_anchors[left];
    const double right_hue = right == 0U
        ? selective_color_hue_anchors.front() + 360.0
        : selective_color_hue_anchors[right];
    const double unwrapped_hue = right == 0U && wrapped_hue < left_hue
        ? wrapped_hue + 360.0
        : wrapped_hue;
    const double position = std::clamp(
        (unwrapped_hue - left_hue) / (right_hue - left_hue),
        0.0,
        1.0
    );
    const double right_weight = 0.5 * (1.0 - std::cos(pi * position));
    weights[left] = 1.0 - right_weight;
    weights[right] = right_weight;
    return weights;
}

[[nodiscard]] double color_range_weight(
    const PerceptualColorRange& range,
    const double hue
) noexcept {
    if (!range.enabled) {
        return 0.0;
    }
    const double distance = std::abs(signed_hue_distance(hue, range.center_degrees));
    const double feather = range.width_degrees * range.softness;
    if (feather == 0.0) {
        return distance <= range.width_degrees ? 1.0 : 0.0;
    }
    const double fully_selected = range.width_degrees - feather;
    return 1.0 - smoothstep(fully_selected, range.width_degrees, distance);
}

[[nodiscard]] bool apply_ordered_color_range(
    Vector3& lab,
    const PerceptualColorRange& range
) noexcept {
    if (!range.enabled) {
        return false;
    }
    const double chroma = std::hypot(lab[1], lab[2]);
    const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
    const double confidence = smoothstep(0.002, 0.02, relative_chroma);
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
    return range.hue_shift_degrees != 0.0 || range.saturation != 0.0
        || range.lightness != 0.0;
}

template <std::size_t Size>
[[nodiscard]] double weighted_sum(
    const std::array<double, Size>& values,
    const std::array<double, Size>& weights
) noexcept {
    double result = 0.0;
    for (std::size_t index = 0U; index < Size; ++index) {
        result += values[index] * weights[index];
    }
    return result;
}

[[nodiscard]] bool normalized_amount(const double value) noexcept {
    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

[[nodiscard]] bool selective_tone_is_neutral(
    const SelectiveToneAdjustment& parameters
) noexcept {
    return parameters.highlights == 0.0 && parameters.shadows == 0.0
        && parameters.whites == 0.0 && parameters.blacks == 0.0;
}

[[nodiscard]] bool perceptual_hue_mapping_is_neutral(
    const PerceptualColorAdjustment& parameters
) noexcept {
    const bool bands_are_neutral = std::ranges::all_of(parameters.hue, [](const double value) {
        return value == 0.0;
    }) && std::ranges::all_of(parameters.saturation, [](const double value) {
        return value == 0.0;
    }) && std::ranges::all_of(parameters.lightness, [](const double value) {
        return value == 0.0;
    });
    const auto range_is_neutral = [](const PerceptualColorRange& range) {
        return !range.enabled
            || (range.hue_shift_degrees == 0.0 && range.saturation == 0.0
                && range.lightness == 0.0);
    };
    const bool ranges_are_neutral = range_is_neutral(parameters.color_range)
        && std::ranges::all_of(parameters.additional_color_ranges, range_is_neutral);
    return parameters.vibrance == 0.0 && bands_are_neutral && ranges_are_neutral;
}

[[nodiscard]] bool perceptual_color_mapping_is_neutral(
    const PerceptualColorAdjustment& parameters
) noexcept {
    return parameters.global_a_balance == 0.0 && parameters.global_b_balance == 0.0
        && perceptual_hue_mapping_is_neutral(parameters);
}

[[nodiscard]] bool color_warper_is_neutral(
    const OklabColorWarperAdjustment& parameters
) noexcept {
    return parameters.strength == 0.0 || std::ranges::all_of(
        parameters.control_points,
        [](const OklabColorWarperControlPoint& point) {
            return point.a_offset == 0.0 && point.b_offset == 0.0;
        }
    );
}

[[nodiscard]] double color_warper_displacement(
    const OklabColorWarperAdjustment& parameters,
    const std::size_t row,
    const std::size_t column,
    const bool a_axis
) noexcept {
    const OklabColorWarperControlPoint& point = parameters.control_points.at(
        row * oklab_color_warper_grid_side + column
    );
    return a_axis ? point.a_offset : point.b_offset;
}

[[nodiscard]] Vector3 apply_oklab_color_warper(
    const Vector3& input,
    const OklabColorWarperAdjustment& parameters,
    const WorkingSpaceTransform& color_transform
) noexcept {
    Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
    const double half_extent = oklab_color_warper_half_extent;
    const double a_distance = half_extent - std::abs(lab[1]);
    const double b_distance = half_extent - std::abs(lab[2]);
    // Do not pull colors arbitrarily far outside the mesh toward an edge
    // control point. A short feather preserves a continuous boundary while
    // keeping the lattice's declared Oklab domain honest.
    constexpr double edge_feather = 0.04;
    const double coverage = smoothstep(0.0, edge_feather, a_distance)
        * smoothstep(0.0, edge_feather, b_distance);
    if (coverage == 0.0 || parameters.strength == 0.0) {
        return input;
    }

    const double coordinate_scale = static_cast<double>(oklab_color_warper_grid_side - 1U);
    const double grid_a = std::clamp(
        (lab[1] + half_extent) / (2.0 * half_extent) * coordinate_scale,
        0.0,
        coordinate_scale
    );
    const double grid_b = std::clamp(
        (lab[2] + half_extent) / (2.0 * half_extent) * coordinate_scale,
        0.0,
        coordinate_scale
    );
    const std::size_t left = static_cast<std::size_t>(std::floor(grid_a));
    const std::size_t top = static_cast<std::size_t>(std::floor(grid_b));
    const std::size_t right = std::min(left + 1U, oklab_color_warper_grid_side - 1U);
    const std::size_t bottom = std::min(top + 1U, oklab_color_warper_grid_side - 1U);
    const double horizontal = grid_a - static_cast<double>(left);
    const double vertical = grid_b - static_cast<double>(top);
    const auto bilinear = [=, &parameters](const bool a_axis) {
        const double top_value = std::lerp(
            color_warper_displacement(parameters, top, left, a_axis),
            color_warper_displacement(parameters, top, right, a_axis),
            horizontal
        );
        const double bottom_value = std::lerp(
            color_warper_displacement(parameters, bottom, left, a_axis),
            color_warper_displacement(parameters, bottom, right, a_axis),
            horizontal
        );
        return std::lerp(top_value, bottom_value, vertical);
    };
    const double amount = coverage * parameters.strength;
    lab[1] += amount * bilinear(true);
    lab[2] += amount * bilinear(false);
    return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
}

// The Oklab axes are perceptually opponent: a is green/red and b is
// blue/yellow.  A broad balance must still leave the absolute black point
// neutral, otherwise minor color adjustments turn the toe into colored noise.
// The smooth low-light protection avoids that instability while keeping white
// and mid-tone balance available for neutralizing a cast.
[[nodiscard]] bool apply_global_oklab_opponent_balance(
    Vector3& lab,
    const PerceptualColorAdjustment& parameters
) noexcept {
    if (parameters.global_a_balance == 0.0 && parameters.global_b_balance == 0.0) {
        return false;
    }
    constexpr double maximum_axis_offset = 0.075;
    const double low_light_protection = smoothstep(0.015, 0.090, std::max(0.0, lab[0]));
    lab[1] += parameters.global_a_balance * maximum_axis_offset * low_light_protection;
    lab[2] += parameters.global_b_balance * maximum_axis_offset * low_light_protection;
    return true;
}

[[nodiscard]] bool selective_color_is_neutral(
    const PerceptualColorAdjustment& parameters
) noexcept {
    return std::ranges::all_of(
        parameters.selective_color_cmyk,
        [](const auto& target) {
            return std::ranges::all_of(target, [](const double value) {
                return value == 0.0;
            });
        }
    );
}

[[nodiscard]] std::array<double, selective_color_target_count>
selective_color_target_weights(const Vector3& lab) noexcept {
    std::array<double, selective_color_target_count> weights{};
    const double lightness = std::clamp(lab[0], 0.0, 1.0);
    const double chroma = std::hypot(lab[1], lab[2]);
    const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
    const double chromatic = smoothstep(0.002, 0.08, relative_chroma);
    if (chromatic > 0.0) {
        const double hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
        const auto hue_weights = selective_color_hue_weights(hue);
        for (std::size_t index = 0U; index < hue_weights.size(); ++index) {
            weights[index] = chromatic * hue_weights[index];
        }
    }
    // Achromatic colors distribute continuously between White, Neutrals, and
    // Blacks. This lets a lightly tinted highlight receive both a chromatic
    // correction and a smaller White correction rather than snapping at an
    // arbitrary hue/chroma boundary.
    const double neutral = 1.0 - chromatic;
    weights[6] = neutral * smoothstep(0.62, 0.94, lightness);
    weights[8] = neutral * (1.0 - smoothstep(0.06, 0.38, lightness));
    weights[7] = std::max(0.0, neutral - weights[6] - weights[8]);
    return weights;
}

[[nodiscard]] Vector3 apply_selective_color(
    const Vector3& input,
    const PerceptualColorAdjustment& parameters,
    const WorkingSpaceTransform& color_transform
) noexcept {
    const Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
    const auto target_weights = selective_color_target_weights(lab);
    std::array<double, selective_color_component_count> adjustment{};
    for (std::size_t target = 0U; target < selective_color_target_count; ++target) {
        for (std::size_t component = 0U;
             component < selective_color_component_count;
             ++component) {
            adjustment[component] += target_weights[target]
                * parameters.selective_color_cmyk[target][component];
        }
    }
    if (std::ranges::all_of(adjustment, [](const double value) { return value == 0.0; })) {
        return input;
    }

    // Selective Color is a display-referred CMYK-style operation, but Shadow
    // keeps its developer in scene-linear working RGB. Normalize around the
    // current scene peak, modify a bounded CMYK proxy, then restore that peak.
    // This makes ordinary RGB/JPEG edits intuitive while avoiding an unwanted
    // hard clip of RAW highlight headroom merely because a color correction ran.
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
    for (std::size_t component = 0U;
         component < selective_color_component_count;
         ++component) {
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
    // Keep the familiar CMYK authoring model, but let the perceptual layer
    // optionally protect Oklab L. This is especially useful when Selective
    // Color is used to neutralize a cast in skin, snow, or a product color:
    // the correction can reshape hue/chroma without unexpectedly changing the
    // perceived exposure chosen earlier in the graph. It deliberately leaves
    // scene-linear values unbounded; output gamut mapping owns clipping.
    if (parameters.selective_color_lightness_protection > 0.0) {
        Vector3 corrected_lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, output));
        corrected_lab[0] = std::lerp(
            corrected_lab[0],
            lab[0],
            parameters.selective_color_lightness_protection
        );
        output = multiply(color_transform.xyz_to_rgb, oklab_to_xyz(corrected_lab));
    }
    return output;
}

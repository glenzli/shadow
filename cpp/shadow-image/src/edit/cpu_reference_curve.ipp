// Included once inside cpu_reference.cpp's private namespace.
// Smooth monotone curve preparation and evaluation are kept together so
// validation, runtime sampling and preview sampling cannot drift apart.

[[nodiscard]] bool same_nonzero_sign(const double left, const double right) noexcept {
    return (left > 0.0 && right > 0.0) || (left < 0.0 && right < 0.0);
}

[[nodiscard]] bool identity_curve_set(const ToneCurveSet& curve) noexcept {
    return curve.points.size() == 2U
        && curve.points[0] == ToneCurvePoint{0.0, 0.0}
        && curve.points[1] == ToneCurvePoint{1.0, 1.0};
}

[[nodiscard]] bool zero_curve_set(const ToneCurveSet& curve) noexcept {
    return std::ranges::all_of(curve.points, [](const ToneCurvePoint point) {
        return point.y == 0.0;
    });
}

[[nodiscard]] double pchip_endpoint_derivative(
    const double first_width,
    const double second_width,
    const double first_slope,
    const double second_slope
) {
    const double numerator = (2.0 * first_width + second_width) * first_slope
        - first_width * second_slope;
    double derivative = numerator / (first_width + second_width);
    if (first_slope == 0.0 || !same_nonzero_sign(derivative, first_slope)) {
        return 0.0;
    }
    if (
        !same_nonzero_sign(first_slope, second_slope)
        && std::abs(derivative) > 3.0 * std::abs(first_slope)
    ) {
        derivative = 3.0 * first_slope;
    }
    return derivative;
}

[[nodiscard]] PreparedSmoothToneCurve prepare_smooth_tone_curve(
    const ToneCurveSet& curve
) {
    if (curve.points.size() < 2U || curve.points.size() > maximum_tone_curve_points) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve must contain between 2 and 256 control points"
        );
    }
    if (curve.points.front().x != 0.0 || curve.points.back().x != 1.0) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve x coordinates must start at zero and end at one"
        );
    }

    std::vector<double> interval_widths;
    std::vector<double> secant_slopes;
    interval_widths.reserve(curve.points.size() - 1U);
    secant_slopes.reserve(curve.points.size() - 1U);
    for (std::size_t index = 0U; index < curve.points.size(); ++index) {
        const ToneCurvePoint point = curve.points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "smooth tone curve control points must contain only finite values"
            );
        }
        if (index == 0U) {
            continue;
        }
        const ToneCurvePoint previous = curve.points[index - 1U];
        const double width = point.x - previous.x;
        if (width <= 0.0) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "smooth tone curve x coordinates must be strictly increasing"
            );
        }
        const double slope = (point.y - previous.y) / width;
        if (!std::isfinite(slope)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "smooth tone curve secant slopes must be finite"
            );
        }
        interval_widths.push_back(width);
        secant_slopes.push_back(slope);
    }

    PreparedSmoothToneCurve prepared{
        .curve = &curve,
        .knot_derivatives = std::vector<double>(curve.points.size(), 0.0),
        .identity = identity_curve_set(curve),
    };
    if (curve.points.size() == 2U) {
        prepared.knot_derivatives[0] = secant_slopes[0];
        prepared.knot_derivatives[1] = secant_slopes[0];
        return prepared;
    }

    prepared.knot_derivatives.front() = pchip_endpoint_derivative(
        interval_widths[0],
        interval_widths[1],
        secant_slopes[0],
        secant_slopes[1]
    );
    for (std::size_t index = 1U; index + 1U < curve.points.size(); ++index) {
        const double previous_slope = secant_slopes[index - 1U];
        const double next_slope = secant_slopes[index];
        // A sign change is an authored local extremum. A zero knot derivative keeps it at
        // that knot rather than inventing an extra oscillation inside either interval.
        if (!same_nonzero_sign(previous_slope, next_slope)) {
            prepared.knot_derivatives[index] = 0.0;
            continue;
        }
        const double previous_width = interval_widths[index - 1U];
        const double next_width = interval_widths[index];
        const double first_weight = 2.0 * next_width + previous_width;
        const double second_weight = next_width + 2.0 * previous_width;
        // Fritsch-Butland's weighted harmonic mean preserves the monotonicity of both
        // adjoining intervals while keeping the first derivative continuous.
        prepared.knot_derivatives[index] = (first_weight + second_weight)
            / (first_weight / previous_slope + second_weight / next_slope);
    }
    const std::size_t last_interval = interval_widths.size() - 1U;
    prepared.knot_derivatives.back() = pchip_endpoint_derivative(
        interval_widths[last_interval],
        interval_widths[last_interval - 1U],
        secant_slopes[last_interval],
        secant_slopes[last_interval - 1U]
    );
    if (!std::ranges::all_of(prepared.knot_derivatives, [](const double derivative) {
            return std::isfinite(derivative);
        })) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve PCHIP derivatives must be finite"
        );
    }
    return prepared;
}

[[nodiscard]] double evaluate_smooth_tone_curve(
    const PreparedSmoothToneCurve& prepared,
    const double value
) {
    if (prepared.identity) {
        return value;
    }
    const auto& points = prepared.curve->points;
    if (value <= points.front().x) {
        return points.front().y
            + (value - points.front().x) * prepared.knot_derivatives.front();
    }
    if (value >= points.back().x) {
        return points.back().y
            + (value - points.back().x) * prepared.knot_derivatives.back();
    }

    const auto upper = std::upper_bound(
        points.begin(),
        points.end(),
        value,
        [](const double sample, const ToneCurvePoint& point) { return sample < point.x; }
    );
    const std::size_t segment = static_cast<std::size_t>(upper - points.begin()) - 1U;
    const ToneCurvePoint left = points[segment];
    const ToneCurvePoint right = points[segment + 1U];
    const double width = right.x - left.x;
    const double t = (value - left.x) / width;
    const double t_squared = t * t;
    const double t_cubed = t_squared * t;
    const double h00 = 2.0 * t_cubed - 3.0 * t_squared + 1.0;
    const double h10 = t_cubed - 2.0 * t_squared + t;
    const double h01 = -2.0 * t_cubed + 3.0 * t_squared;
    const double h11 = t_cubed - t_squared;
    return h00 * left.y + h10 * width * prepared.knot_derivatives[segment]
        + h01 * right.y + h11 * width * prepared.knot_derivatives[segment + 1U];
}

[[nodiscard]] float checked_tone_curve_float(const double value) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "tone curve pixel result exceeded finite float32 range"
        );
    }
    return static_cast<float>(value);
}

[[nodiscard]] PreparedSmoothToneCurve prepare_oklab_lightness_tone_curve(
    const OklabLightnessToneCurve& curve
) {
    if (
        curve.parameter_schema_version != oklab_lightness_tone_curve_parameter_schema_version
        || curve.implementation_version != oklab_lightness_tone_curve_implementation_version
    ) {
        throw EditError(
            EditErrorCode::unsupported_version,
            std::nullopt,
            "Oklab lightness curve supports only parameter schema 1 and implementation 1"
        );
    }
    return prepare_smooth_tone_curve(curve.lightness);
}

template <typename CheckedConversion>
void apply_prepared_oklab_lightness_tone_curve(
    FloatRgbImage& image,
    const PreparedSmoothToneCurve& prepared,
    const WorkingSpaceTransform& color_transform,
    CheckedConversion&& checked_conversion
) {
    if (prepared.identity) {
        return;
    }
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    for (std::uint32_t y = 0U; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0U; x < image.dimensions.width; ++x) {
            const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
            const Vector3 input{
                static_cast<double>(image.samples[sample]),
                static_cast<double>(image.samples[sample + 1U]),
                static_cast<double>(image.samples[sample + 2U]),
            };
            Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
            // a and b deliberately remain untouched.  This is the key semantic
            // distinction from RGB master/channel curves: it changes perceived
            // lightness without directly rotating hue or scaling chroma.
            lab[0] = evaluate_smooth_tone_curve(prepared, lab[0]);
            const Vector3 output = multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                image.samples[sample + channel] = checked_conversion(output[channel]);
            }
        }
    }
}

[[nodiscard]] PreparedSmoothToneCurve prepare_oklab_lightness_tone_curve_node(
    const OklabLightnessToneCurve& curve,
    const AdjustmentNode& node,
    const std::size_t index
) {
    try {
        return prepare_oklab_lightness_tone_curve(curve);
    } catch (const EditError& error) {
        throw_node_error(error.code(), index, node, error.what());
    }
}

inline constexpr double maximum_oklab_opponent_curve_offset = 0.12;

[[nodiscard]] PreparedOklabOpponentToneCurves prepare_oklab_opponent_tone_curves(
    const OklabOpponentToneCurves& curves
) {
    if (curves.parameter_schema_version != oklab_opponent_tone_curve_parameter_schema_version
        || curves.implementation_version != oklab_opponent_tone_curve_implementation_version) {
        throw EditError(
            EditErrorCode::unsupported_version,
            std::nullopt,
            "Oklab opponent curves support only parameter schema 1 and implementation 1"
        );
    }
    const auto prepare_axis = [](const ToneCurveSet& curve, const std::string_view axis) {
        if (!std::ranges::all_of(curve.points, [](const ToneCurvePoint point) {
                return point.y >= -maximum_oklab_opponent_curve_offset
                    && point.y <= maximum_oklab_opponent_curve_offset;
            })) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "Oklab opponent " + std::string(axis)
                    + " curve offsets must remain within the declared perceptual range"
            );
        }
        PreparedSmoothToneCurve prepared = prepare_smooth_tone_curve(curve);
        // Oklab opponent curves are offset curves, so their neutral form is
        // y=0 rather than the usual y=x tone-curve identity.
        prepared.identity = zero_curve_set(curve);
        return prepared;
    };
    return PreparedOklabOpponentToneCurves{
        .a = prepare_axis(curves.a, "a"),
        .b = prepare_axis(curves.b, "b"),
    };
}

template <typename CheckedConversion>
void apply_prepared_oklab_opponent_tone_curves(
    FloatRgbImage& image,
    const PreparedOklabOpponentToneCurves& prepared,
    const WorkingSpaceTransform& color_transform,
    CheckedConversion&& checked_conversion
) {
    if (prepared.identity()) {
        return;
    }
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    parallel_for_rows(image.dimensions.height, [&](const std::uint32_t first_row,
                                                   const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = y * stride;
            for (std::size_t x = 0U; x < image.dimensions.width; ++x) {
                const std::size_t sample = row + x * rgb_channels;
                const Vector3 input{
                    static_cast<double>(image.samples[sample]),
                    static_cast<double>(image.samples[sample + 1U]),
                    static_cast<double>(image.samples[sample + 2U]),
                };
                Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
                // The authored control domain is photographic Oklab L. HDR
                // headroom should not extrapolate into an arbitrary cast, so
                // highlights beyond that domain use the endpoint color shift.
                const double key = std::clamp(lab[0], 0.0, 1.0);
                lab[1] += std::clamp(
                    evaluate_smooth_tone_curve(prepared.a, key),
                    -maximum_oklab_opponent_curve_offset,
                    maximum_oklab_opponent_curve_offset
                );
                lab[2] += std::clamp(
                    evaluate_smooth_tone_curve(prepared.b, key),
                    -maximum_oklab_opponent_curve_offset,
                    maximum_oklab_opponent_curve_offset
                );
                const Vector3 output = multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
                for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                    image.samples[sample + channel] = checked_conversion(output[channel]);
                }
            }
        }
    });
}

[[nodiscard]] PreparedOklabOpponentToneCurves prepare_oklab_opponent_tone_curves_node(
    const OklabOpponentToneCurves& curves,
    const AdjustmentNode& node,
    const std::size_t index
) {
    try {
        return prepare_oklab_opponent_tone_curves(curves);
    } catch (const EditError& error) {
        throw_node_error(error.code(), index, node, error.what());
    }
}

#pragma once

namespace shadow::image::detail {

// DCP HueSatMap/LookTable/ProfileToneCurve executes after the camera matrix in DCP's ProPhoto
// working space. Tables and curve coefficients are immutable buffers prepared for one image.
inline constexpr char metal_dcp_color_source[] = R"METAL(
// DCP HueSatMap/LookTable/ProfileToneCurve executes after the camera matrix, in DCP's
// ProPhoto working space.  Tables and curve coefficients are immutable buffers prepared by the
// host for one image.  This preserves the CPU reference's HDR behaviour: normalize a positive
// super-white triplet, apply bounded DCP operations, then restore its measured peak.
struct DcpHsvDelta {
    float hue_shift_degrees;
    float saturation_scale;
    float value_scale;
};

struct DcpToneCurvePoint {
    float input;
    float output;
    float second_derivative;
};

struct DcpPostParameters {
    uint pixel_count;
    uint hue_hue_divisions;
    uint hue_saturation_divisions;
    uint hue_value_divisions;
    uint hue_encoding_srgb;
    uint look_hue_divisions;
    uint look_saturation_divisions;
    uint look_value_divisions;
    uint look_encoding_srgb;
    uint tone_curve_count;
    float srgb_to_working[9];
    float working_to_srgb[9];
};

inline float dcp_unit(const float value) {
    return clamp(value, 0.0f, 1.0f);
}

inline float3 dcp_matrix_multiply(constant float* matrix, const float3 value) {
    return float3(
        matrix[0] * value.x + matrix[1] * value.y + matrix[2] * value.z,
        matrix[3] * value.x + matrix[4] * value.y + matrix[5] * value.z,
        matrix[6] * value.x + matrix[7] * value.y + matrix[8] * value.z
    );
}

inline float dcp_srgb_encode(const float linear) {
    const float value = dcp_unit(linear);
    return value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float dcp_srgb_decode(const float encoded) {
    const float value = dcp_unit(encoded);
    return value <= 0.04045f
        ? value / 12.92f
        : pow((value + 0.055f) / 1.055f, 2.4f);
}

struct DcpHsv {
    float hue;
    float saturation;
    float value;
};

inline DcpHsv dcp_rgb_to_hsv(const float3 input) {
    const float3 rgb = clamp(input, 0.0f, 1.0f);
    const float maximum = max(rgb.x, max(rgb.y, rgb.z));
    const float minimum = min(rgb.x, min(rgb.y, rgb.z));
    const float chroma = maximum - minimum;
    DcpHsv result{0.0f, 0.0f, maximum};
    if (maximum <= 1.0e-12f || chroma <= 1.0e-12f) {
        return result;
    }
    result.saturation = chroma / maximum;
    if (maximum == rgb.x) {
        result.hue = (rgb.y - rgb.z) / chroma;
    } else if (maximum == rgb.y) {
        result.hue = 2.0f + (rgb.z - rgb.x) / chroma;
    } else {
        result.hue = 4.0f + (rgb.x - rgb.y) / chroma;
    }
    result.hue = fmod(result.hue / 6.0f + 1.0f, 1.0f);
    return result;
}

inline float3 dcp_hsv_to_rgb(const DcpHsv hsv) {
    const float hue = fmod(hsv.hue + 1.0f, 1.0f) * 6.0f;
    const float saturation = dcp_unit(hsv.saturation);
    const float value = dcp_unit(hsv.value);
    const float chroma = value * saturation;
    const float intermediate = chroma * (1.0f - fabs(fmod(hue, 2.0f) - 1.0f));
    const float match = value - chroma;
    if (hue < 1.0f) {
        return float3(chroma + match, intermediate + match, match);
    }
    if (hue < 2.0f) {
        return float3(intermediate + match, chroma + match, match);
    }
    if (hue < 3.0f) {
        return float3(match, chroma + match, intermediate + match);
    }
    if (hue < 4.0f) {
        return float3(match, intermediate + match, chroma + match);
    }
    if (hue < 5.0f) {
        return float3(intermediate + match, match, chroma + match);
    }
    return float3(chroma + match, match, intermediate + match);
}

inline DcpHsvDelta dcp_sample_hsv_table(
    device const DcpHsvDelta* table,
    const uint hue_divisions,
    const uint saturation_divisions,
    const uint value_divisions,
    const DcpHsv hsv
) {
    const float hue_coordinate = hsv.hue * float(hue_divisions);
    const uint hue0 = uint(floor(hue_coordinate)) % hue_divisions;
    const uint hue1 = (hue0 + 1u) % hue_divisions;
    const float hue_fraction = hue_coordinate - floor(hue_coordinate);
    const float saturation_coordinate = dcp_unit(hsv.saturation)
        * float(saturation_divisions - 1u);
    const uint saturation0 = uint(floor(saturation_coordinate));
    const uint saturation1 = min(saturation0 + 1u, saturation_divisions - 1u);
    const float saturation_fraction = saturation_coordinate - floor(saturation_coordinate);
    const float value_coordinate = dcp_unit(hsv.value) * float(value_divisions - 1u);
    const uint value0 = uint(floor(value_coordinate));
    const uint value1 = min(value0 + 1u, value_divisions - 1u);
    const float value_fraction = value_coordinate - floor(value_coordinate);

    float hue_sine = 0.0f;
    float hue_cosine = 0.0f;
    float saturation_scale = 0.0f;
    float value_scale = 0.0f;
    for (uint value_choice = 0u; value_choice < 2u; ++value_choice) {
        const uint value = value_choice == 0u ? value0 : value1;
        const float value_weight = value_choice == 0u ? 1.0f - value_fraction : value_fraction;
        for (uint hue_choice = 0u; hue_choice < 2u; ++hue_choice) {
            const uint hue = hue_choice == 0u ? hue0 : hue1;
            const float hue_weight = hue_choice == 0u ? 1.0f - hue_fraction : hue_fraction;
            for (uint saturation_choice = 0u; saturation_choice < 2u; ++saturation_choice) {
                const uint saturation = saturation_choice == 0u ? saturation0 : saturation1;
                const float saturation_weight = saturation_choice == 0u
                    ? 1.0f - saturation_fraction : saturation_fraction;
                const float weight = value_weight * hue_weight * saturation_weight;
                const DcpHsvDelta delta = table[
                    ((value * hue_divisions) + hue) * saturation_divisions + saturation
                ];
                const float radians = delta.hue_shift_degrees * 0.01745329251994329577f;
                hue_sine += sin(radians) * weight;
                hue_cosine += cos(radians) * weight;
                saturation_scale += delta.saturation_scale * weight;
                value_scale += delta.value_scale * weight;
            }
        }
    }
    return DcpHsvDelta{
        atan2(hue_sine, hue_cosine) * 57.295779513082320876f,
        saturation_scale,
        value_scale,
    };
}

inline float3 dcp_apply_hsv_table(
    const float3 input,
    device const DcpHsvDelta* table,
    const uint hue_divisions,
    const uint saturation_divisions,
    const uint value_divisions,
    const uint encoding_srgb
) {
    DcpHsv hsv = dcp_rgb_to_hsv(input);
    if (encoding_srgb != 0u) {
        hsv.value = dcp_srgb_encode(hsv.value);
    }
    const DcpHsvDelta delta = dcp_sample_hsv_table(
        table,
        hue_divisions,
        saturation_divisions,
        value_divisions,
        hsv
    );
    hsv.hue = fmod(hsv.hue + delta.hue_shift_degrees / 360.0f + 1.0f, 1.0f);
    hsv.saturation = dcp_unit(hsv.saturation * delta.saturation_scale);
    hsv.value = dcp_unit(hsv.value * delta.value_scale);
    if (encoding_srgb != 0u) {
        hsv.value = dcp_srgb_decode(hsv.value);
    }
    return dcp_hsv_to_rgb(hsv);
}

inline float dcp_sample_tone_curve(
    device const DcpToneCurvePoint* points,
    const uint point_count,
    const float input
) {
    const float value = dcp_unit(input);
    if (point_count == 0u) {
        return value;
    }
    if (value <= points[0].input) {
        return dcp_unit(points[0].output);
    }
    if (value >= points[point_count - 1u].input) {
        return dcp_unit(points[point_count - 1u].output);
    }
    uint left = 0u;
    uint right = point_count - 1u;
    while (right - left > 1u) {
        const uint middle = left + (right - left) / 2u;
        if (points[middle].input <= value) {
            left = middle;
        } else {
            right = middle;
        }
    }
    const float width = points[right].input - points[left].input;
    if (width <= 0.0f) {
        return dcp_unit(points[left].output);
    }
    const float a = (points[right].input - value) / width;
    const float b = (value - points[left].input) / width;
    const float output = a * points[left].output + b * points[right].output
        + ((a * a * a - a) * points[left].second_derivative
            + (b * b * b - b) * points[right].second_derivative)
            * width * width / 6.0f;
    return dcp_unit(output);
}

kernel void develop_dcp_post_matrix(
    device float* pixels [[buffer(0)]],
    device const DcpHsvDelta* hue_table [[buffer(1)]],
    device const DcpHsvDelta* look_table [[buffer(2)]],
    device const DcpToneCurvePoint* tone_curve [[buffer(3)]],
    constant DcpPostParameters& parameters [[buffer(4)]],
    uint pixel_index [[thread_position_in_grid]]
) {
    if (pixel_index >= parameters.pixel_count) {
        return;
    }
    const uint index = pixel_index * 3u;
    const float3 source = float3(pixels[index], pixels[index + 1u], pixels[index + 2u]);
    if (!all(isfinite(source))) {
        return;
    }
    float3 working = dcp_matrix_multiply(parameters.srgb_to_working, source);
    if (!all(isfinite(working))) {
        return;
    }
    const bool bounded = all(working >= float3(0.0f)) && all(working <= float3(1.0f));
    float peak = 1.0f;
    if (!bounded) {
        peak = max(working.x, max(working.y, working.z));
        if (peak <= 0.0f || any(working < float3(0.0f))) {
            return;
        }
        working /= peak;
    }
    if (parameters.hue_hue_divisions != 0u) {
        working = dcp_apply_hsv_table(
            working,
            hue_table,
            parameters.hue_hue_divisions,
            parameters.hue_saturation_divisions,
            parameters.hue_value_divisions,
            parameters.hue_encoding_srgb
        );
    }
    if (parameters.look_hue_divisions != 0u) {
        working = dcp_apply_hsv_table(
            working,
            look_table,
            parameters.look_hue_divisions,
            parameters.look_saturation_divisions,
            parameters.look_value_divisions,
            parameters.look_encoding_srgb
        );
    }
    if (parameters.tone_curve_count != 0u) {
        working = float3(
            dcp_sample_tone_curve(tone_curve, parameters.tone_curve_count, working.x),
            dcp_sample_tone_curve(tone_curve, parameters.tone_curve_count, working.y),
            dcp_sample_tone_curve(tone_curve, parameters.tone_curve_count, working.z)
        );
    }
    if (!bounded) {
        working *= peak;
    }
    const float3 result = dcp_matrix_multiply(parameters.working_to_srgb, working);
    if (!all(isfinite(result))) {
        return;
    }
    pixels[index] = result.x;
    pixels[index + 1u] = result.y;
    pixels[index + 2u] = result.z;
}
)METAL";

} // namespace shadow::image::detail

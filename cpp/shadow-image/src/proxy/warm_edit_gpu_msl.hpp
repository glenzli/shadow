#pragma once

#include <string_view>

namespace shadow::image::detail {

inline constexpr std::string_view warm_kernel_source = R"METAL(
struct WarmDisplayParameters {
    uint output_origin_x;
    uint output_origin_y;
    uint apply_scene_curve;
    uint retain_linear;
};

// This first Metal neighbourhood stage deliberately keeps the working image in scene-linear
// RGB. It retains the CPU denoiser's luma/chroma decomposition, but folds the local statistics
// into one resident, edge-aware pass so slider updates do not have to round-trip a proxy through
// system memory.
struct WarmDenoiseParameters {
    uint width;
    uint height;
    uint radius;
    uint reserved;
    float luminance_strength;
    float color_strength;
    float spatial_sigma;
    float edge_sigma;
    float red_luminance;
    float green_luminance;
    float blue_luminance;
    float reserved_1;
};

struct WarmSharpenParameters {
    uint width;
    uint height;
    uint horizontal_radius;
    uint vertical_radius;
    float sigma_x;
    float sigma_y;
    float amount;
    float threshold_ev;
    float masking;
    float red_luminance;
    float green_luminance;
    float blue_luminance;
};

struct WarmTextureParameters {
    uint width;
    uint height;
    uint horizontal_radius;
    uint vertical_radius;
    float sigma_x;
    float sigma_y;
    float amount;
    float reserved;
};

struct WarmGaussianParameters {
    uint width;
    uint height;
    uint horizontal_radius;
    uint vertical_radius;
    float sigma_x;
    float sigma_y;
    float reserved_0;
    float reserved_1;
};

struct WarmClarityParameters {
    uint width;
    uint height;
    uint vertical_radius;
    uint reserved;
    float sigma_y;
    float amount;
    float reserved_0;
    float reserved_1;
};

struct WarmDehazeDefringeParameters {
    float dehaze;
    float purple_amount;
    float green_amount;
    float purple_hue_low;
    float purple_hue_high;
    float green_hue_low;
    float green_hue_high;
    float red_luminance;
    float green_luminance;
    float blue_luminance;
    float reserved_0;
    float reserved_1;
};

struct WarmTextureClarityParameters {
    uint width;
    uint height;
    uint vertical_radius;
    uint reserved;
    float sigma_y;
    float texture_amount;
    float clarity_amount;
    float reserved_0;
};

// Local Contrast uses the CPU reference's two self-guided box filters.  It is
// deliberately a separate resident stage from Gaussian Texture/Clarity: the
// repeated box means preserve real luminance boundaries at a much broader
// photographic support without introducing a preview-only approximation.
struct WarmBoxParameters {
    uint width;
    uint height;
    uint radius;
    uint reserved;
};

struct WarmGuidedCoefficientsParameters {
    uint width;
    uint height;
    float epsilon;
    float reserved;
};

struct WarmLocalContrastParameters {
    uint width;
    uint height;
    float amount;
    float reserved;
};

inline float3 linear_srgb_to_oklab(float3 rgb) {
    const float l = signed_cbrt(
        0.4122214708f * rgb.r + 0.5363325363f * rgb.g + 0.0514459929f * rgb.b
    );
    const float m = signed_cbrt(
        0.2119034982f * rgb.r + 0.6806995451f * rgb.g + 0.1073969566f * rgb.b
    );
    const float s = signed_cbrt(
        0.0883024619f * rgb.r + 0.2817188376f * rgb.g + 0.6299787005f * rgb.b
    );
    return float3(
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    );
}

inline float3 oklab_to_linear_srgb(float3 lab) {
    const float l_root = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const float m_root = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const float s_root = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const float l = l_root * l_root * l_root;
    const float m = m_root * m_root * m_root;
    const float s = s_root * s_root * s_root;
    return float3(
        4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s
    );
}

inline bool inside_display_srgb(float3 rgb) {
    return isfinite(rgb.r) && isfinite(rgb.g) && isfinite(rgb.b)
        && rgb.r >= 0.0f && rgb.r <= 1.0f
        && rgb.g >= 0.0f && rgb.g <= 1.0f
        && rgb.b >= 0.0f && rgb.b <= 1.0f;
}

inline float scene_luminance_to_display_luminance(float luminance) {
    if (!(luminance > 0.0f)) {
        return 0.0f;
    }
    const float maximum_safe_luminance = 1.0e6f;
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    const float scene = min(luminance, maximum_safe_luminance);
    const float curved =
        scene * (a * scene + b) / (scene * (c * scene + d) + e);
    return clamp(curved / (a / c), 0.0f, 1.0f);
}

inline float3 neutral_scene_display_curve(float3 input) {
    const float luminance =
        input.r * 0.2126f + input.g * 0.7152f + input.b * 0.0722f;
    if (!(luminance > 0.0f)) {
        return input;
    }
    return input * (scene_luminance_to_display_luminance(luminance) / luminance);
}

inline float3 map_display_gamut(float3 input, bool apply_scene_curve) {
    const float3 display_linear = apply_scene_curve
        ? neutral_scene_display_curve(input)
        : input;
    if (inside_display_srgb(display_linear)) {
        return display_linear;
    }

    float3 lab = linear_srgb_to_oklab(display_linear);
    lab.x = clamp(lab.x, 0.0f, 1.0f);
    const float chroma = length(lab.yz);
    float3 best = oklab_to_linear_srgb(float3(lab.x, 0.0f, 0.0f));
    if (!isfinite(chroma) || chroma <= 1.0e-15f) {
        return clamp(best, 0.0f, 1.0f);
    }

    const float2 direction = lab.yz / chroma;
    float lower = 0.0f;
    float upper = min(chroma, 0.5f);
    for (uint iteration = 0u; iteration < 16u; ++iteration) {
        const float candidate_chroma = (lower + upper) * 0.5f;
        const float3 candidate = oklab_to_linear_srgb(float3(
            lab.x,
            direction.x * candidate_chroma,
            direction.y * candidate_chroma
        ));
        if (inside_display_srgb(candidate)) {
            lower = candidate_chroma;
            best = candidate;
        } else {
            upper = candidate_chroma;
        }
    }
    return clamp(best, 0.0f, 1.0f);
}

inline float display_dither(uint x, uint y) {
    uint state = x * 0x9e3779b9u ^ y * 0x85ebca6bu;
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    const float unit = float(state) / float(0xffffffffu);
    return (unit - 0.5f) * 0.90f;
}

inline uchar encode_srgb8(float linear_sample, float dither) {
    const float linear = clamp(linear_sample, 0.0f, 1.0f);
    const float encoded = linear <= 0.0031308f
        ? 12.92f * linear
        : 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;
    return uchar(clamp(floor(encoded * 255.0f + dither + 0.5f), 0.0f, 255.0f));
}

kernel void render_warm_preview_v1(
    device const float* source [[buffer(0)]],
    device float* adjusted [[buffer(1)]],
    device uchar* display_rgb8 [[buffer(2)]],
    device const MetalAdjustmentOp* operations [[buffer(3)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(4)]],
    constant WarmDisplayParameters& display [[buffer(5)]],
    device MetalAdjustmentStatus& status [[buffer(6)]],
    device const MetalCurveSegment* curve_segments [[buffer(7)]],
    device const float4* lut_entries [[buffer(8)]],
    device const float4* perceptual_mixer_entries [[buffer(9)]],
    device const MetalPerceptualRange* perceptual_range_entries [[buffer(10)]],
    device const float4* selective_color_entries [[buffer(11)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    if (invocation.abi_version != parameter_abi_version
        || invocation.plan_identity_version != plan_identity_version) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        return;
    }

    const uint input_index =
        position.y * invocation.input_row_floats + position.x * 3u;
    float3 rgb = float3(
        source[input_index],
        source[input_index + 1u],
        source[input_index + 2u]
    );
    if (!execute_adjustment_program(
            rgb,
            operations,
            curve_segments,
            lut_entries,
            perceptual_mixer_entries,
            perceptual_range_entries,
            selective_color_entries,
            invocation,
            status
        )) {
        return;
    }

    if (display.retain_linear != 0u) {
        const uint adjusted_index =
            position.y * invocation.output_row_floats + position.x * 3u;
        adjusted[adjusted_index] = rgb.x;
        adjusted[adjusted_index + 1u] = rgb.y;
        adjusted[adjusted_index + 2u] = rgb.z;
    }

    const float3 mapped = map_display_gamut(
        rgb,
        display.apply_scene_curve != 0u
    );
    const float dither = display_dither(
        display.output_origin_x + position.x,
        display.output_origin_y + position.y
    );
    const uint output_index =
        (position.y * invocation.width + position.x) * 3u;
    display_rgb8[output_index] = encode_srgb8(mapped.r, dither);
    display_rgb8[output_index + 1u] = encode_srgb8(mapped.g, dither);
    display_rgb8[output_index + 2u] = encode_srgb8(mapped.b, dither);
}

kernel void execute_warm_adjustment_v1(
    device const float* source [[buffer(0)]],
    device float* adjusted [[buffer(1)]],
    device const MetalAdjustmentOp* operations [[buffer(3)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(4)]],
    device MetalAdjustmentStatus& status [[buffer(6)]],
    device const MetalCurveSegment* curve_segments [[buffer(7)]],
    device const float4* lut_entries [[buffer(8)]],
    device const float4* perceptual_mixer_entries [[buffer(9)]],
    device const MetalPerceptualRange* perceptual_range_entries [[buffer(10)]],
    device const float4* selective_color_entries [[buffer(11)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    if (invocation.abi_version != parameter_abi_version
        || invocation.plan_identity_version != plan_identity_version) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        return;
    }

    const uint input_index =
        position.y * invocation.input_row_floats + position.x * 3u;
    float3 rgb = float3(
        source[input_index],
        source[input_index + 1u],
        source[input_index + 2u]
    );
    if (!execute_adjustment_program(
            rgb,
            operations,
            curve_segments,
            lut_entries,
            perceptual_mixer_entries,
            perceptual_range_entries,
            selective_color_entries,
            invocation,
            status
        )) {
        return;
    }

    const uint output_index =
        position.y * invocation.output_row_floats + position.x * 3u;
    adjusted[output_index] = rgb.x;
    adjusted[output_index + 1u] = rgb.y;
    adjusted[output_index + 2u] = rgb.z;
}

kernel void guided_denoise_warm_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmDenoiseParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint width = parameters.width;
    const uint output_index = (position.y * width + position.x) * 3u;
    const float3 centre = float3(
        input[output_index], input[output_index + 1u], input[output_index + 2u]
    );
    const float centre_luma = centre.r * parameters.red_luminance
        + centre.g * parameters.green_luminance
        + centre.b * parameters.blue_luminance;
    const float centre_red_chroma = centre.r - centre_luma;
    const float centre_blue_chroma = centre.b - centre_luma;
    const int radius = int(parameters.radius);
    const float spatial_denominator = max(
        2.0f * parameters.spatial_sigma * parameters.spatial_sigma,
        1.0e-8f
    );
    const float edge_denominator = max(
        2.0f * parameters.edge_sigma * parameters.edge_sigma,
        1.0e-8f
    );

    float weight_sum = 0.0f;
    float luma_sum = 0.0f;
    float red_chroma_sum = 0.0f;
    float blue_chroma_sum = 0.0f;
    for (int offset_y = -4; offset_y <= 4; ++offset_y) {
        if (abs(offset_y) > radius) {
            continue;
        }
        const uint sample_y = uint(clamp(
            int(position.y) + offset_y,
            0,
            int(parameters.height) - 1
        ));
        for (int offset_x = -4; offset_x <= 4; ++offset_x) {
            if (abs(offset_x) > radius) {
                continue;
            }
            const uint sample_x = uint(clamp(
                int(position.x) + offset_x,
                0,
                int(parameters.width) - 1
            ));
            const uint sample_index = (sample_y * width + sample_x) * 3u;
            const float3 sample = float3(
                input[sample_index],
                input[sample_index + 1u],
                input[sample_index + 2u]
            );
            const float sample_luma = sample.r * parameters.red_luminance
                + sample.g * parameters.green_luminance
                + sample.b * parameters.blue_luminance;
            const float luma_delta = sample_luma - centre_luma;
            const float distance_squared = float(offset_x * offset_x + offset_y * offset_y);
            const float weight = exp(-distance_squared / spatial_denominator)
                * exp(-(luma_delta * luma_delta) / edge_denominator);
            weight_sum += weight;
            luma_sum += weight * sample_luma;
            red_chroma_sum += weight * (sample.r - sample_luma);
            blue_chroma_sum += weight * (sample.b - sample_luma);
        }
    }
    const float inverse_weight = 1.0f / max(weight_sum, 1.0e-8f);
    const float filtered_luma = luma_sum * inverse_weight;
    const float filtered_red_chroma = red_chroma_sum * inverse_weight;
    const float filtered_blue_chroma = blue_chroma_sum * inverse_weight;
    const float output_luma = mix(
        centre_luma,
        filtered_luma,
        clamp(parameters.luminance_strength, 0.0f, 1.0f)
    );
    const float output_red_chroma = mix(
        centre_red_chroma,
        filtered_red_chroma,
        clamp(parameters.color_strength, 0.0f, 1.0f)
    );
    const float output_blue_chroma = mix(
        centre_blue_chroma,
        filtered_blue_chroma,
        clamp(parameters.color_strength, 0.0f, 1.0f)
    );
    output[output_index] = output_luma + output_red_chroma;
    output[output_index + 1u] = output_luma
        - (parameters.red_luminance * output_red_chroma
            + parameters.blue_luminance * output_blue_chroma)
            / max(parameters.green_luminance, 1.0e-6f);
    output[output_index + 2u] = output_luma + output_blue_chroma;
}

inline uint warm_reflect101_coordinate(int coordinate, uint extent) {
    if (extent <= 1u) {
        return 0u;
    }
    const int signed_extent = int(extent);
    while (coordinate < 0 || coordinate >= signed_extent) {
        coordinate = coordinate < 0
            ? -coordinate
            : 2 * signed_extent - 2 - coordinate;
    }
    return uint(coordinate);
}

// The CPU guided filter replicates image borders for every box mean. Keep that
// exact boundary convention here; reflect-101 is intentionally reserved for
// the Gaussian Texture/Clarity kernels above.
inline uint warm_clamp_coordinate(int coordinate, uint extent) {
    if (extent <= 1u) {
        return 0u;
    }
    return uint(clamp(coordinate, 0, int(extent) - 1));
}

// Keep the sharpening analysis scalar. The RGB gain is reconstructed only in the final pass,
// exactly as in the CPU capture-sharpening operator, so fine detail cannot create a color halo.
kernel void warm_sharpen_log_luminance_v1(
    device const float* input [[buffer(0)]],
    device float* log_luminance [[buffer(1)]],
    constant WarmSharpenParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint rgb_index = (position.y * parameters.width + position.x) * 3u;
    const float luma = input[rgb_index] * parameters.red_luminance
        + input[rgb_index + 1u] * parameters.green_luminance
        + input[rgb_index + 2u] * parameters.blue_luminance;
    log_luminance[position.y * parameters.width + position.x] = log2(max(luma, 5.9604645e-8f));
}

kernel void warm_sharpen_horizontal_v1(
    device const float* input [[buffer(0)]],
    device float* horizontal [[buffer(1)]],
    constant WarmSharpenParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.horizontal_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_x * parameters.sigma_x,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_x = warm_reflect101_coordinate(int(position.x) + offset, parameters.width);
        weighted_sum += input[position.y * parameters.width + sample_x] * weight;
        weight_sum += weight;
    }
    horizontal[position.y * parameters.width + position.x] = weighted_sum / max(weight_sum, 1.0e-12f);
}

kernel void warm_sharpen_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* horizontal [[buffer(1)]],
    device float* output [[buffer(2)]],
    constant WarmSharpenParameters& parameters [[buffer(3)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.vertical_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_y * parameters.sigma_y,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_y = warm_reflect101_coordinate(int(position.y) + offset, parameters.height);
        weighted_sum += horizontal[sample_y * parameters.width + position.x] * weight;
        weight_sum += weight;
    }
    const uint rgb_index = (position.y * parameters.width + position.x) * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    const float luma = rgb.r * parameters.red_luminance
        + rgb.g * parameters.green_luminance + rgb.b * parameters.blue_luminance;
    const float blurred = weighted_sum / max(weight_sum, 1.0e-12f);
    const float detail = log2(max(luma, 5.9604645e-8f)) - blurred;
    const float thresholded = copysign(max(0.0f, abs(detail) - parameters.threshold_ev), detail);
    const float edge_confidence = smoothstep(
        parameters.threshold_ev,
        parameters.threshold_ev + 0.25f,
        abs(detail)
    );
    const float mask = (1.0f - parameters.masking)
        + parameters.masking * edge_confidence;
    const float gain = luma <= 5.9604645e-8f || thresholded == 0.0f
        ? 1.0f
        : exp2(parameters.amount * thresholded * mask);
    output[rgb_index] = rgb.r * gain;
    output[rgb_index + 1u] = rgb.g * gain;
    output[rgb_index + 2u] = rgb.b * gain;
}

// Texture follows the CPU perceptual-detail contract: analyse only Oklab L, soften its compact
// base band, then add a shadow-protected and compressed residual before converting to the
// original working RGB space. This deliberately leaves Oklab a/b untouched.
kernel void warm_texture_lightness_v1(
    device const float* input [[buffer(0)]],
    device float* lightness [[buffer(1)]],
    constant WarmTextureParameters& parameters [[buffer(2)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(3)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint rgb_index = (position.y * parameters.width + position.x) * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    lightness[position.y * parameters.width + position.x] =
        working_rgb_to_oklab(rgb, invocation).x;
}

kernel void warm_texture_horizontal_v1(
    device const float* input [[buffer(0)]],
    device float* horizontal [[buffer(1)]],
    constant WarmTextureParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.horizontal_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_x * parameters.sigma_x,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_x = warm_reflect101_coordinate(int(position.x) + offset, parameters.width);
        weighted_sum += input[position.y * parameters.width + sample_x] * weight;
        weight_sum += weight;
    }
    horizontal[position.y * parameters.width + position.x] = weighted_sum / max(weight_sum, 1.0e-12f);
}

kernel void warm_texture_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* horizontal [[buffer(1)]],
    device float* output [[buffer(2)]],
    constant WarmTextureParameters& parameters [[buffer(3)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(4)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.vertical_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_y * parameters.sigma_y,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_y = warm_reflect101_coordinate(int(position.y) + offset, parameters.height);
        weighted_sum += horizontal[sample_y * parameters.width + position.x] * weight;
        weight_sum += weight;
    }
    const uint rgb_index = (position.y * parameters.width + position.x) * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    float3 lab = working_rgb_to_oklab(rgb, invocation);
    const float base = weighted_sum / max(weight_sum, 1.0e-12f);
    const float residual = lab.x - base;
    const float compressed = residual / (1.0f + abs(residual) / 0.035f);
    const float shadow_protection = adjustment_smoothstep(0.015f, 0.090f, lab.x);
    lab.x += parameters.amount * 0.70f * compressed * shadow_protection;
    const float3 adjusted = oklab_to_working_rgb(lab, invocation);
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}

kernel void warm_scalar_vertical_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmGaussianParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.vertical_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_y * parameters.sigma_y,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_y = warm_reflect101_coordinate(int(position.y) + offset, parameters.height);
        weighted_sum += input[sample_y * parameters.width + position.x] * weight;
        weight_sum += weight;
    }
    output[position.y * parameters.width + position.x] =
        weighted_sum / max(weight_sum, 1.0e-12f);
}

// Clarity is the protected mid-frequency Oklab-L band from the CPU reference. The compact
// lightness raster stays resident while a large Gaussian is evaluated, so this path changes
// structure without chroma shifts or a host-side raster round trip.
kernel void warm_clarity_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* small_base [[buffer(1)]],
    device const float* large_horizontal [[buffer(2)]],
    device float* output [[buffer(3)]],
    constant WarmClarityParameters& parameters [[buffer(4)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(5)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.vertical_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_y * parameters.sigma_y,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_y = warm_reflect101_coordinate(int(position.y) + offset, parameters.height);
        weighted_sum += large_horizontal[sample_y * parameters.width + position.x] * weight;
        weight_sum += weight;
    }
    const uint pixel = position.y * parameters.width + position.x;
    const uint rgb_index = pixel * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    float3 lab = working_rgb_to_oklab(rgb, invocation);
    const float small = small_base[pixel];
    const float large = weighted_sum / max(weight_sum, 1.0e-12f);
    const float high_frequency = lab.x - small;
    const float mid_frequency = small - large;
    const float edge_protection = 1.0f - adjustment_smoothstep(
        0.018f,
        0.085f,
        abs(high_frequency)
    );
    const float shadow_protection = adjustment_smoothstep(0.015f, 0.090f, lab.x);
    const float compressed = mid_frequency / (1.0f + abs(mid_frequency) / 0.090f);
    lab.x += parameters.amount * 1.15f * compressed
        * edge_protection * shadow_protection;
    const float3 adjusted = oklab_to_working_rgb(lab, invocation);
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}

inline float warm_wrap_degrees(float degrees) {
    const float wrapped = fmod(degrees, 360.0f);
    return wrapped < 0.0f ? wrapped + 360.0f : wrapped;
}

// The dehaze / defringe reference operator is pixel-local. Keep it in the technical stage so
// a slider change never forces the warm preview through a CPU proxy, while preserving the exact
// CPU ordering before any creative grade or LUT.
kernel void warm_dehaze_defringe_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmDehazeDefringeParameters& parameters [[buffer(2)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(3)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    const uint rgb_index = (position.y * invocation.width + position.x) * 3u;
    float3 rgb = float3(input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]);
    const float luma = rgb.r * parameters.red_luminance
        + rgb.g * parameters.green_luminance + rgb.b * parameters.blue_luminance;
    if (parameters.dehaze > 0.0f) {
        const float veil = max(0.0f, min(rgb.r, min(rgb.g, rgb.b)));
        const float maximum = max(0.0f, max(rgb.r, max(rgb.g, rgb.b)));
        const float veil_fraction = clamp(veil / (maximum + 0.18f), 0.0f, 1.0f);
        const float transmission = max(
            0.2f,
            1.0f - 0.88f * parameters.dehaze * veil_fraction
        );
        rgb = (rgb - parameters.dehaze * 0.65f * veil) / transmission;
    } else if (parameters.dehaze < 0.0f) {
        const float amount = -parameters.dehaze;
        const float atmosphere = max(0.18f, luma + 0.28f);
        rgb = mix(rgb, float3(atmosphere), 0.55f * amount);
    }
    float3 lab = working_rgb_to_oklab(rgb, invocation);
    const float chroma = length(lab.yz);
    if ((parameters.purple_amount > 0.0f || parameters.green_amount > 0.0f)
        && chroma > 1.0e-8f) {
        const float hue = warm_wrap_degrees(atan2(lab.z, lab.y) * 57.2957795131f);
        const float purple_weight = adjustment_smoothstep(
            parameters.purple_hue_low - 10.0f,
            parameters.purple_hue_low,
            hue
        ) * (1.0f - adjustment_smoothstep(
            parameters.purple_hue_high,
            parameters.purple_hue_high + 10.0f,
            hue
        ));
        const float green_weight = adjustment_smoothstep(
            parameters.green_hue_low - 10.0f,
            parameters.green_hue_low,
            hue
        ) * (1.0f - adjustment_smoothstep(
            parameters.green_hue_high,
            parameters.green_hue_high + 10.0f,
            hue
        ));
        const float reduction = max(
            parameters.purple_amount * purple_weight,
            parameters.green_amount * green_weight
        );
        lab.yz *= 1.0f - 0.9f * reduction;
    }
    const float3 adjusted = oklab_to_working_rgb(lab, invocation);
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}

kernel void warm_texture_clarity_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* texture_base [[buffer(1)]],
    device const float* clarity_small [[buffer(2)]],
    device const float* clarity_large_horizontal [[buffer(3)]],
    device float* output [[buffer(4)]],
    constant WarmTextureClarityParameters& parameters [[buffer(5)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(6)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.vertical_radius);
    const float inverse_two_sigma_squared = 1.0f / max(
        2.0f * parameters.sigma_y * parameters.sigma_y,
        1.0e-12f
    );
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    for (int offset = -15; offset <= 15; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
        const uint sample_y = warm_reflect101_coordinate(int(position.y) + offset, parameters.height);
        weighted_sum += clarity_large_horizontal[sample_y * parameters.width + position.x] * weight;
        weight_sum += weight;
    }
    const uint pixel = position.y * parameters.width + position.x;
    const uint rgb_index = pixel * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    float3 lab = working_rgb_to_oklab(rgb, invocation);
    const float shadow_protection = adjustment_smoothstep(0.015f, 0.090f, lab.x);
    const float texture_residual = lab.x - texture_base[pixel];
    lab.x += parameters.texture_amount * 0.70f
        * (texture_residual / (1.0f + abs(texture_residual) / 0.035f))
        * shadow_protection;
    const float high_frequency = lab.x - clarity_small[pixel];
    const float mid_frequency = clarity_small[pixel]
        - weighted_sum / max(weight_sum, 1.0e-12f);
    const float edge_protection = 1.0f - adjustment_smoothstep(
        0.018f, 0.085f, abs(high_frequency)
    );
    lab.x += parameters.clarity_amount * 1.15f
        * (mid_frequency / (1.0f + abs(mid_frequency) / 0.090f))
        * edge_protection * shadow_protection;
    const float3 adjusted = oklab_to_working_rgb(lab, invocation);
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}

// A fixed loop bound keeps the preview-stage cost predictable. CPU-side stage
// preparation declines a radius above this bound and replays the exact CPU
// oracle instead of silently truncating the requested support.
kernel void warm_box_horizontal_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmBoxParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    float sum = 0.0f;
    const int radius = int(parameters.radius);
    for (int offset = -32; offset <= 32; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const uint sample_x = warm_clamp_coordinate(
            int(position.x) + offset,
            parameters.width
        );
        sum += input[position.y * parameters.width + sample_x];
    }
    output[position.y * parameters.width + position.x] = sum
        / float(radius * 2 + 1);
}

kernel void warm_box_vertical_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmBoxParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    float sum = 0.0f;
    const int radius = int(parameters.radius);
    for (int offset = -32; offset <= 32; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const uint sample_y = warm_clamp_coordinate(
            int(position.y) + offset,
            parameters.height
        );
        sum += input[sample_y * parameters.width + position.x];
    }
    output[position.y * parameters.width + position.x] = sum
        / float(radius * 2 + 1);
}

kernel void warm_scalar_square_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmBoxParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint pixel = position.y * parameters.width + position.x;
    output[pixel] = input[pixel] * input[pixel];
}

kernel void warm_guided_coefficients_v1(
    device const float* guide [[buffer(0)]],
    device const float* mean [[buffer(1)]],
    device const float* variance [[buffer(2)]],
    device float* output_a [[buffer(3)]],
    device float* output_b [[buffer(4)]],
    constant WarmGuidedCoefficientsParameters& parameters [[buffer(5)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint pixel = position.y * parameters.width + position.x;
    const float stable_variance = max(
        0.0f,
        variance[pixel] - mean[pixel] * mean[pixel]
    );
    const float a = stable_variance / max(
        stable_variance + parameters.epsilon,
        1.0e-12f
    );
    output_a[pixel] = a;
    output_b[pixel] = mean[pixel] * (1.0f - a);
}

kernel void warm_guided_combine_v1(
    device const float* guide [[buffer(0)]],
    device const float* mean_a [[buffer(1)]],
    device const float* mean_b [[buffer(2)]],
    device float* output [[buffer(3)]],
    constant WarmBoxParameters& parameters [[buffer(4)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint pixel = position.y * parameters.width + position.x;
    output[pixel] = mean_a[pixel] * guide[pixel] + mean_b[pixel];
}

kernel void warm_local_contrast_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* small [[buffer(1)]],
    device const float* large [[buffer(2)]],
    device float* output [[buffer(3)]],
    constant WarmLocalContrastParameters& parameters [[buffer(4)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(5)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint pixel = position.y * parameters.width + position.x;
    const uint rgb_index = pixel * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    float3 lab = working_rgb_to_oklab(rgb, invocation);
    const float broad_residual = small[pixel] - large[pixel];
    const float edge_residual = lab.x - small[pixel];
    const float edge_protection = 1.0f - adjustment_smoothstep(
        0.030f,
        0.120f,
        abs(edge_residual)
    );
    const float shadow_protection = adjustment_smoothstep(0.015f, 0.090f, lab.x);
    const float compressed = broad_residual
        / (1.0f + abs(broad_residual) / 0.115f);
    lab.x += parameters.amount * 1.20f * compressed
        * edge_protection * shadow_protection;
    const float3 adjusted = oklab_to_working_rgb(lab, invocation);
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}
)METAL";

} // namespace shadow::image::detail

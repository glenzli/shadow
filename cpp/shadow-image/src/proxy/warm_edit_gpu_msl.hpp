#pragma once

#include <string_view>

namespace shadow::image::detail {

inline constexpr std::string_view warm_kernel_source_prefix = R"METAL(
struct WarmDisplayParameters {
    uint output_origin_x;
    uint output_origin_y;
    uint apply_scene_curve;
    uint retain_linear;
};

struct WarmLayerBlendParameters {
    uint width;
    uint height;
    uint input_row_floats;
    uint use_precomputed_coverage;
    uint origin_x;
    uint origin_y;
    uint full_width;
    uint full_height;
    uint mask_kind;
    uint invert;
    float opacity;
    float x0;
    float y0;
    float x1;
    float y1;
    float radius_x;
    float radius_y;
    float feather;
    uint brush_grid_columns;
    uint brush_grid_rows;
    uint brush_capsule_count;
    uint brush_reference_count;
    uint reserved_1;
    uint reserved_2;
    float4 rgb_to_xyz_row_0;
    float4 rgb_to_xyz_row_1;
    float4 rgb_to_xyz_row_2;
};

struct WarmBrushCapsule {
    float x0;
    float y0;
    float x1;
    float y1;
};

struct WarmBrushCellRange {
    uint offset;
    uint count;
};

struct WarmRetouchRegionParameters {
    uint width;
    uint height;
    uint input_row_floats;
    uint reserved_0;
    uint bounds_origin_x;
    uint bounds_origin_y;
    uint bounds_width;
    uint bounds_height;
    uint grid_columns;
    uint grid_rows;
    uint capsule_count;
    uint reference_count;
    uint mode;
    uint statistics_group_count;
    uint poisson_iterations;
    uint robust_pass;
    float radius_x;
    float radius_y;
    float donor_offset_x;
    float donor_offset_y;
    float feather;
    float screening_weight;
    float strength;
    float reserved_3;
};

struct WarmRetouchCapsule {
    float x0;
    float y0;
    float x1;
    float y1;
};

struct WarmRetouchCellRange {
    uint offset;
    uint count;
};

struct WarmRetouchStatistics {
    float4 donor_sum_count;
    float4 boundary_sum_count;
    float4 donor_square_sum;
    float4 boundary_square_sum;
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

struct WarmCreativeDetailParameters {
    uint width;
    uint height;
    uint vertical_radius;
    uint reserved;
    float sigma_y;
    float texture_amount;
    float clarity_amount;
    float local_contrast_amount;
};

// Local Contrast uses the CPU reference's two self-guided box filters. Its prepared rasters join
// Gaussian Texture/Clarity only in the final creative-detail kernel so all three bands preserve
// the CPU's one-conversion order.
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

struct WarmSelectiveToneParameters {
    uint width;
    uint height;
    uint reserved_0;
    uint reserved_1;
    float highlights;
    float shadows;
    float whites;
    float blacks;
    float red_luminance;
    float green_luminance;
    float blue_luminance;
    float reserved_2;
    float4 rgb_to_xyz_row_0;
    float4 rgb_to_xyz_row_1;
    float4 rgb_to_xyz_row_2;
    float4 xyz_to_rgb_row_0;
    float4 xyz_to_rgb_row_1;
    float4 xyz_to_rgb_row_2;
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

)METAL";

// Repair/Clone is a distinct runtime-compiled language module. Keeping its fragment separate
// prevents the general warm-adjustment source from becoming one over-limit string literal and
// gives retouch kernels one searchable semantic owner even though all fragments share a library.
inline constexpr std::string_view warm_retouch_kernel_source = R"METAL(
inline float warm_retouch_capsule_distance(
    float2 point,
    WarmRetouchCapsule capsule,
    float2 radius
) {
    const float2 scaled_point = point / radius;
    const float2 start = float2(capsule.x0, capsule.y0) / radius;
    const float2 end = float2(capsule.x1, capsule.y1) / radius;
    const float2 direction = end - start;
    const float denominator = dot(direction, direction);
    const float projection = denominator <= 2.220446049250313e-16f
        ? 0.0f
        : clamp(dot(scaled_point - start, direction) / denominator, 0.0f, 1.0f);
    return length(scaled_point - (start + projection * direction));
}

inline float3 warm_retouch_sample_bilinear(
    device const float* input,
    float2 point,
    constant WarmRetouchRegionParameters& parameters
) {
    const float x = clamp(point.x, 0.0f, float(parameters.width - 1u));
    const float y = clamp(point.y, 0.0f, float(parameters.height - 1u));
    const uint x0 = uint(floor(x));
    const uint y0 = uint(floor(y));
    const uint x1 = min(x0 + 1u, parameters.width - 1u);
    const uint y1 = min(y0 + 1u, parameters.height - 1u);
    const float blend_x = x - float(x0);
    const float blend_y = y - float(y0);
    const uint top_left = y0 * parameters.input_row_floats + x0 * 3u;
    const uint top_right = y0 * parameters.input_row_floats + x1 * 3u;
    const uint bottom_left = y1 * parameters.input_row_floats + x0 * 3u;
    const uint bottom_right = y1 * parameters.input_row_floats + x1 * 3u;
    const float3 top = mix(
        float3(input[top_left], input[top_left + 1u], input[top_left + 2u]),
        float3(input[top_right], input[top_right + 1u], input[top_right + 2u]),
        blend_x
    );
    const float3 bottom = mix(
        float3(input[bottom_left], input[bottom_left + 1u], input[bottom_left + 2u]),
        float3(
            input[bottom_right],
            input[bottom_right + 1u],
            input[bottom_right + 2u]
        ),
        blend_x
    );
    return mix(top, bottom, blend_y);
}

inline float warm_retouch_coverage(
    int2 signed_position,
    constant WarmRetouchRegionParameters& parameters,
    device const WarmRetouchCapsule* capsules,
    device const WarmRetouchCellRange* cells,
    device const uint* references
) {
    if (signed_position.x < 0 || signed_position.y < 0
        || signed_position.x >= int(parameters.width)
        || signed_position.y >= int(parameters.height)) {
        return 0.0f;
    }
    const uint2 position = uint2(signed_position);
    const bool inside_bounds =
        position.x >= parameters.bounds_origin_x
        && position.y >= parameters.bounds_origin_y
        && position.x - parameters.bounds_origin_x < parameters.bounds_width
        && position.y - parameters.bounds_origin_y < parameters.bounds_height;
    if (!inside_bounds) {
        return 0.0f;
    }
    if (parameters.grid_columns == 0u || parameters.grid_rows == 0u
        || parameters.capsule_count == 0u
        || !(parameters.radius_x > 0.0f) || !(parameters.radius_y > 0.0f)) {
        return -1.0f;
    }
    const uint local_x = position.x - parameters.bounds_origin_x;
    const uint local_y = position.y - parameters.bounds_origin_y;
    const uint column = min(
        local_x * parameters.grid_columns / parameters.bounds_width,
        parameters.grid_columns - 1u
    );
    const uint row = min(
        local_y * parameters.grid_rows / parameters.bounds_height,
        parameters.grid_rows - 1u
    );
    const WarmRetouchCellRange range =
        cells[row * parameters.grid_columns + column];
    if (range.offset > parameters.reference_count
        || range.count > parameters.reference_count - range.offset) {
        return -1.0f;
    }
    float distance = 3.402823466e+38f;
    for (uint candidate = 0u; candidate < range.count; ++candidate) {
        const uint capsule_index = references[range.offset + candidate];
        if (capsule_index >= parameters.capsule_count) {
            return -1.0f;
        }
        distance = min(
            distance,
            warm_retouch_capsule_distance(
                float2(position),
                capsules[capsule_index],
                float2(parameters.radius_x, parameters.radius_y)
            )
        );
    }
    if (distance > 1.0f) {
        return 0.0f;
    }
    return parameters.feather <= 0.0f
        ? 1.0f
        : 1.0f - smoothstep(1.0f - parameters.feather, 1.0f, distance);
}

kernel void warm_retouch_clone_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmRetouchRegionParameters& parameters [[buffer(2)]],
    device MetalAdjustmentStatus& status [[buffer(3)]],
    device const WarmRetouchCapsule* capsules [[buffer(4)]],
    device const WarmRetouchCellRange* cells [[buffer(5)]],
    device const uint* references [[buffer(6)]],
    uint2 local_position [[thread_position_in_grid]]
) {
    const uint2 position = local_position
        + uint2(parameters.bounds_origin_x, parameters.bounds_origin_y);
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint input_index =
        position.y * parameters.input_row_floats + position.x * 3u;
    const uint output_index =
        (position.y * parameters.width + position.x) * 3u;
    const float3 original = float3(
        input[input_index],
        input[input_index + 1u],
        input[input_index + 2u]
    );
    const float coverage = warm_retouch_coverage(
        int2(position),
        parameters,
        capsules,
        cells,
        references
    );
    if (coverage < 0.0f) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        return;
    }
    float3 result = original;
    if (coverage > 0.0f) {
        const float3 donor = warm_retouch_sample_bilinear(
            input,
            float2(position)
                + float2(parameters.donor_offset_x, parameters.donor_offset_y),
            parameters
        );
        result = fma(float3(coverage * parameters.strength), donor - original, original);
    }
    if (!all(isfinite(result))) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    output[output_index] = result.x;
    output[output_index + 1u] = result.y;
    output[output_index + 2u] = result.z;
}

kernel void warm_retouch_heal_statistics_v1(
    device const float* source [[buffer(0)]],
    constant WarmRetouchRegionParameters& parameters [[buffer(1)]],
    device MetalAdjustmentStatus& status [[buffer(2)]],
    device const WarmRetouchCapsule* capsules [[buffer(3)]],
    device const WarmRetouchCellRange* cells [[buffer(4)]],
    device const uint* references [[buffer(5)]],
    device WarmRetouchStatistics* partials [[buffer(6)]],
    device const WarmRetouchStatistics& summary [[buffer(7)]],
    uint index [[thread_position_in_grid]],
    uint group_index [[threadgroup_position_in_grid]],
    ushort lane [[thread_index_in_threadgroup]],
    ushort group_size [[threads_per_threadgroup]]
) {
    threadgroup float4 donor_values[256];
    threadgroup float4 boundary_values[256];
    threadgroup float4 donor_squares[256];
    threadgroup float4 boundary_squares[256];
    float4 donor = float4(0.0f);
    float4 boundary = float4(0.0f);
    float4 donor_square = float4(0.0f);
    float4 boundary_square = float4(0.0f);
    const uint pixel_count = parameters.bounds_width * parameters.bounds_height;
    if (index < pixel_count) {
        const uint2 position = uint2(
            parameters.bounds_origin_x + index % parameters.bounds_width,
            parameters.bounds_origin_y + index / parameters.bounds_width
        );
        const float coverage = warm_retouch_coverage(
            int2(position),
            parameters,
            capsules,
            cells,
            references
        );
        if (coverage < 0.0f) {
            report_adjustment_failure(status, status_bad_resource, 0u);
        } else {
            const float minimum_coverage = 1.0e-4f;
            const float3 donor_sample = warm_retouch_sample_bilinear(
                source,
                float2(position)
                    + float2(parameters.donor_offset_x, parameters.donor_offset_y),
                parameters
            );
            if (coverage <= minimum_coverage) {
                const int2 signed_position = int2(position);
                const float left = warm_retouch_coverage(
                    signed_position + int2(-1, 0),
                    parameters,
                    capsules,
                    cells,
                    references
                );
                const float right = warm_retouch_coverage(
                    signed_position + int2(1, 0),
                    parameters,
                    capsules,
                    cells,
                    references
                );
                const float up = warm_retouch_coverage(
                    signed_position + int2(0, -1),
                    parameters,
                    capsules,
                    cells,
                    references
                );
                const float down = warm_retouch_coverage(
                    signed_position + int2(0, 1),
                    parameters,
                    capsules,
                    cells,
                    references
                );
                if (min(min(left, right), min(up, down)) < 0.0f) {
                    report_adjustment_failure(status, status_bad_resource, 0u);
                } else if (max(max(left, right), max(up, down)) > minimum_coverage) {
                    const uint sample =
                        position.y * parameters.input_row_floats + position.x * 3u;
                    float3 retained_donor = donor_sample;
                    float3 retained_boundary = float3(
                        source[sample],
                        source[sample + 1u],
                        source[sample + 2u]
                    );
                    if (parameters.robust_pass != 0u) {
                        if (summary.donor_sum_count.w > 0.0f) {
                            retained_donor = clamp(
                                retained_donor,
                                summary.donor_sum_count.xyz
                                    - 2.5f * summary.donor_square_sum.xyz,
                                summary.donor_sum_count.xyz
                                    + 2.5f * summary.donor_square_sum.xyz
                            );
                        }
                        if (summary.boundary_sum_count.w > 0.0f) {
                            retained_boundary = clamp(
                                retained_boundary,
                                summary.boundary_sum_count.xyz
                                    - 2.5f * summary.boundary_square_sum.xyz,
                                summary.boundary_sum_count.xyz
                                    + 2.5f * summary.boundary_square_sum.xyz
                            );
                        }
                    }
                    donor = float4(retained_donor, 1.0f);
                    donor_square = float4(retained_donor * retained_donor, 0.0f);
                    boundary = float4(retained_boundary, 1.0f);
                    boundary_square = float4(
                        retained_boundary * retained_boundary,
                        0.0f
                    );
                }
            }
        }
    }
    donor_values[lane] = donor;
    boundary_values[lane] = boundary;
    donor_squares[lane] = donor_square;
    boundary_squares[lane] = boundary_square;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (ushort stride = group_size / 2; stride > 0; stride /= 2) {
        if (lane < stride) {
            donor_values[lane] += donor_values[lane + stride];
            boundary_values[lane] += boundary_values[lane + stride];
            donor_squares[lane] += donor_squares[lane + stride];
            boundary_squares[lane] += boundary_squares[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0) {
        partials[group_index] = WarmRetouchStatistics{
            donor_values[0],
            boundary_values[0],
            donor_squares[0],
            boundary_squares[0]
        };
    }
}

kernel void warm_retouch_heal_reduce_v1(
    device const WarmRetouchStatistics* partials [[buffer(0)]],
    device WarmRetouchStatistics& summary [[buffer(1)]],
    constant WarmRetouchRegionParameters& parameters [[buffer(2)]],
    uint index [[thread_position_in_grid]]
) {
    if (index != 0u) {
        return;
    }
    WarmRetouchStatistics total{
        float4(0.0f),
        float4(0.0f),
        float4(0.0f),
        float4(0.0f)
    };
    for (uint group = 0u; group < parameters.statistics_group_count; ++group) {
        total.donor_sum_count += partials[group].donor_sum_count;
        total.boundary_sum_count += partials[group].boundary_sum_count;
        total.donor_square_sum += partials[group].donor_square_sum;
        total.boundary_square_sum += partials[group].boundary_square_sum;
    }
    const float donor_count = total.donor_sum_count.w;
    const float boundary_count = total.boundary_sum_count.w;
    const float3 donor_mean =
        donor_count > 0.0f ? total.donor_sum_count.xyz / donor_count : float3(0.0f);
    const float3 boundary_mean = boundary_count > 0.0f
        ? total.boundary_sum_count.xyz / boundary_count
        : float3(0.0f);
    const float3 donor_standard_deviation =
        parameters.robust_pass == 0u && donor_count > 0.0f
        ? sqrt(max(
            total.donor_square_sum.xyz / donor_count - donor_mean * donor_mean,
            float3(0.0f)
        ))
        : float3(0.0f);
    const float3 boundary_standard_deviation =
        parameters.robust_pass == 0u && boundary_count > 0.0f
        ? sqrt(max(
            total.boundary_square_sum.xyz / boundary_count
                - boundary_mean * boundary_mean,
            float3(0.0f)
        ))
        : float3(0.0f);
    summary = WarmRetouchStatistics{
        float4(donor_mean, donor_count),
        float4(boundary_mean, boundary_count),
        float4(donor_standard_deviation, 0.0f),
        float4(boundary_standard_deviation, 0.0f)
    };
}

kernel void warm_retouch_heal_initialize_v1(
    device const float* source [[buffer(0)]],
    device float* solution [[buffer(1)]],
    constant WarmRetouchRegionParameters& parameters [[buffer(2)]],
    device MetalAdjustmentStatus& status [[buffer(3)]],
    device const WarmRetouchCapsule* capsules [[buffer(4)]],
    device const WarmRetouchCellRange* cells [[buffer(5)]],
    device const uint* references [[buffer(6)]],
    device const WarmRetouchStatistics& summary [[buffer(7)]],
    uint2 local_position [[thread_position_in_grid]]
) {
    const uint2 position = local_position
        + uint2(parameters.bounds_origin_x, parameters.bounds_origin_y);
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint source_index =
        position.y * parameters.input_row_floats + position.x * 3u;
    const uint output_index = (position.y * parameters.width + position.x) * 3u;
    const float3 original = float3(
        source[source_index],
        source[source_index + 1u],
        source[source_index + 2u]
    );
    const float coverage = warm_retouch_coverage(
        int2(position),
        parameters,
        capsules,
        cells,
        references
    );
    if (coverage < 0.0f) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        return;
    }
    float3 result = original;
    if (coverage > 1.0e-4f && summary.donor_sum_count.w > 0.0f
        && summary.boundary_sum_count.w > 0.0f) {
        result = warm_retouch_sample_bilinear(
            source,
            float2(position)
                + float2(parameters.donor_offset_x, parameters.donor_offset_y),
            parameters
        ) + summary.boundary_sum_count.xyz - summary.donor_sum_count.xyz;
    }
    if (!all(isfinite(result))) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    solution[output_index] = result.x;
    solution[output_index + 1u] = result.y;
    solution[output_index + 2u] = result.z;
}

kernel void warm_retouch_heal_jacobi_v1(
    device const float* source [[buffer(0)]],
    device const float* current [[buffer(1)]],
    device float* next [[buffer(2)]],
    constant WarmRetouchRegionParameters& parameters [[buffer(3)]],
    device MetalAdjustmentStatus& status [[buffer(4)]],
    device const WarmRetouchCapsule* capsules [[buffer(5)]],
    device const WarmRetouchCellRange* cells [[buffer(6)]],
    device const uint* references [[buffer(7)]],
    device const WarmRetouchStatistics& summary [[buffer(8)]],
    uint2 local_position [[thread_position_in_grid]]
) {
    const uint2 position = local_position
        + uint2(parameters.bounds_origin_x, parameters.bounds_origin_y);
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint source_index =
        position.y * parameters.input_row_floats + position.x * 3u;
    const uint packed_index = (position.y * parameters.width + position.x) * 3u;
    const float3 original = float3(
        source[source_index],
        source[source_index + 1u],
        source[source_index + 2u]
    );
    const float coverage = warm_retouch_coverage(
        int2(position),
        parameters,
        capsules,
        cells,
        references
    );
    if (coverage < 0.0f) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        return;
    }
    if (coverage <= 1.0e-4f || summary.donor_sum_count.w <= 0.0f
        || summary.boundary_sum_count.w <= 0.0f) {
        next[packed_index] = original.x;
        next[packed_index + 1u] = original.y;
        next[packed_index + 2u] = original.z;
        return;
    }

    const float3 donor_center = warm_retouch_sample_bilinear(
        source,
        float2(position) + float2(parameters.donor_offset_x, parameters.donor_offset_y),
        parameters
    );
    float3 neighbor_sum = float3(0.0f);
    float3 donor_laplacian = float3(0.0f);
    uint neighbor_count = 0u;
    const int2 signed_position = int2(position);
    constexpr int2 offsets[4] = {
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1)
    };
    for (uint neighbor = 0u; neighbor < 4u; ++neighbor) {
        const int2 adjacent = signed_position + offsets[neighbor];
        if (adjacent.x < 0 || adjacent.y < 0
            || adjacent.x >= int(parameters.width)
            || adjacent.y >= int(parameters.height)) {
            continue;
        }
        const float adjacent_coverage = warm_retouch_coverage(
            adjacent,
            parameters,
            capsules,
            cells,
            references
        );
        if (adjacent_coverage < 0.0f) {
            report_adjustment_failure(status, status_bad_resource, 0u);
            return;
        }
        const uint adjacent_source =
            uint(adjacent.y) * parameters.input_row_floats + uint(adjacent.x) * 3u;
        const uint adjacent_packed =
            (uint(adjacent.y) * parameters.width + uint(adjacent.x)) * 3u;
        neighbor_sum += adjacent_coverage > 1.0e-4f
            ? float3(
                current[adjacent_packed],
                current[adjacent_packed + 1u],
                current[adjacent_packed + 2u]
            )
            : float3(
                source[adjacent_source],
                source[adjacent_source + 1u],
                source[adjacent_source + 2u]
            );
        donor_laplacian += donor_center - warm_retouch_sample_bilinear(
            source,
            float2(adjacent)
                + float2(parameters.donor_offset_x, parameters.donor_offset_y),
            parameters
        );
        ++neighbor_count;
    }
    const float3 screened_target = donor_center
        + summary.boundary_sum_count.xyz - summary.donor_sum_count.xyz;
    const float denominator =
        float(neighbor_count) + parameters.screening_weight;
    const float3 result = neighbor_count == 0u
        ? screened_target
        : (neighbor_sum + donor_laplacian
            + parameters.screening_weight * screened_target) / denominator;
    if (!all(isfinite(result))) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    next[packed_index] = result.x;
    next[packed_index + 1u] = result.y;
    next[packed_index + 2u] = result.z;
}

kernel void warm_retouch_heal_blend_v1(
    device const float* source [[buffer(0)]],
    device const float* solution [[buffer(1)]],
    device float* output [[buffer(2)]],
    constant WarmRetouchRegionParameters& parameters [[buffer(3)]],
    device MetalAdjustmentStatus& status [[buffer(4)]],
    device const WarmRetouchCapsule* capsules [[buffer(5)]],
    device const WarmRetouchCellRange* cells [[buffer(6)]],
    device const uint* references [[buffer(7)]],
    device const WarmRetouchStatistics& summary [[buffer(8)]],
    uint2 local_position [[thread_position_in_grid]]
) {
    const uint2 position = local_position
        + uint2(parameters.bounds_origin_x, parameters.bounds_origin_y);
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint source_index =
        position.y * parameters.input_row_floats + position.x * 3u;
    const uint packed_index = (position.y * parameters.width + position.x) * 3u;
    const float3 original = float3(
        source[source_index],
        source[source_index + 1u],
        source[source_index + 2u]
    );
    const float coverage = warm_retouch_coverage(
        int2(position),
        parameters,
        capsules,
        cells,
        references
    );
    if (coverage < 0.0f) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        return;
    }
    const float alpha = summary.donor_sum_count.w > 0.0f
            && summary.boundary_sum_count.w > 0.0f
        ? clamp(coverage * parameters.strength, 0.0f, 1.0f)
        : 0.0f;
    const float3 healed = float3(
        solution[packed_index],
        solution[packed_index + 1u],
        solution[packed_index + 2u]
    );
    const float3 result = mix(original, healed, alpha);
    if (!all(isfinite(result))) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    output[packed_index] = result.x;
    output[packed_index + 1u] = result.y;
    output[packed_index + 2u] = result.z;
}

)METAL";

inline constexpr std::string_view warm_kernel_source_suffix = R"METAL(
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
    for (int offset = -radius; offset <= radius; ++offset) {
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
    for (int offset = -radius; offset <= radius; ++offset) {
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
    for (int offset = -radius; offset <= radius; ++offset) {
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
    for (int offset = -radius; offset <= radius; ++offset) {
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
    for (int offset = -radius; offset <= radius; ++offset) {
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
    for (int offset = -radius; offset <= radius; ++offset) {
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

kernel void warm_creative_detail_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* texture_base [[buffer(1)]],
    device const float* clarity_small [[buffer(2)]],
    device const float* clarity_large_horizontal [[buffer(3)]],
    device const float* local_contrast_small [[buffer(4)]],
    device const float* local_contrast_large [[buffer(5)]],
    device float* output [[buffer(6)]],
    constant WarmCreativeDetailParameters& parameters [[buffer(7)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(8)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const int radius = int(parameters.vertical_radius);
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    if (parameters.clarity_amount != 0.0f) {
        const float inverse_two_sigma_squared = 1.0f / max(
            2.0f * parameters.sigma_y * parameters.sigma_y,
            1.0e-12f
        );
        for (int offset = -radius; offset <= radius; ++offset) {
            const float weight = exp(-float(offset * offset) * inverse_two_sigma_squared);
            const uint sample_y = warm_reflect101_coordinate(
                int(position.y) + offset,
                parameters.height
            );
            weighted_sum += clarity_large_horizontal[
                sample_y * parameters.width + position.x
            ] * weight;
            weight_sum += weight;
        }
    }
    const uint pixel = position.y * parameters.width + position.x;
    const uint rgb_index = pixel * 3u;
    const float3 rgb = float3(
        input[rgb_index], input[rgb_index + 1u], input[rgb_index + 2u]
    );
    float3 lab = working_rgb_to_oklab(rgb, invocation);
    const float shadow_protection = adjustment_smoothstep(0.015f, 0.090f, lab.x);
    if (parameters.texture_amount != 0.0f) {
        const float texture_residual = lab.x - texture_base[pixel];
        lab.x += parameters.texture_amount * 0.70f
            * (texture_residual / (1.0f + abs(texture_residual) / 0.035f))
            * shadow_protection;
    }
    if (parameters.clarity_amount != 0.0f) {
        const float high_frequency = lab.x - clarity_small[pixel];
        const float mid_frequency = clarity_small[pixel]
            - weighted_sum / max(weight_sum, 1.0e-12f);
        const float edge_protection = 1.0f - adjustment_smoothstep(
            0.018f,
            0.085f,
            abs(high_frequency)
        );
        lab.x += parameters.clarity_amount * 1.15f
            * (mid_frequency / (1.0f + abs(mid_frequency) / 0.090f))
            * edge_protection * shadow_protection;
    }
    if (parameters.local_contrast_amount != 0.0f) {
        const float broad_residual =
            local_contrast_small[pixel] - local_contrast_large[pixel];
        const float edge_residual = lab.x - local_contrast_small[pixel];
        const float edge_protection = 1.0f - adjustment_smoothstep(
            0.030f,
            0.120f,
            abs(edge_residual)
        );
        const float compressed = broad_residual
            / (1.0f + abs(broad_residual) / 0.115f);
        lab.x += parameters.local_contrast_amount * 1.20f * compressed
            * edge_protection * shadow_protection;
    }
    const float3 adjusted = oklab_to_working_rgb(lab, invocation);
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}

inline void warm_compensated_add(
    float value,
    thread float& sum,
    thread float& correction
) {
    const float adjusted = value - correction;
    const float next = sum + adjusted;
    correction = (next - sum) - adjusted;
    sum = next;
}

// One thread owns one complete row. After the bounded first window, each output advances with
// one entering and one leaving sample. This keeps full-resolution Local Contrast O(pixels)
// instead of repeating an O(radius) sum independently at every pixel.
kernel void warm_box_horizontal_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmBoxParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x != 0u || position.y >= parameters.height) {
        return;
    }
    const uint row_offset = position.y * parameters.width;
    float sum = 0.0f;
    float correction = 0.0f;
    const int radius = int(parameters.radius);
    for (int offset = -radius; offset <= radius; ++offset) {
        const uint sample_x = warm_clamp_coordinate(offset, parameters.width);
        warm_compensated_add(input[row_offset + sample_x], sum, correction);
    }
    const float divisor = float(radius * 2 + 1);
    for (uint x = 0u; x < parameters.width; ++x) {
        output[row_offset + x] = sum / divisor;
        if (x + 1u >= parameters.width) {
            continue;
        }
        const uint leaving_x = warm_clamp_coordinate(int(x) - radius, parameters.width);
        const uint entering_x = warm_clamp_coordinate(
            int(x) + radius + 1,
            parameters.width
        );
        warm_compensated_add(-input[row_offset + leaving_x], sum, correction);
        warm_compensated_add(input[row_offset + entering_x], sum, correction);
    }
}

kernel void warm_selective_tone_guide_v1(
    device const float* input [[buffer(0)]],
    device float* guide [[buffer(1)]],
    constant WarmSelectiveToneParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint pixel = position.y * parameters.width + position.x;
    const uint rgb_index = pixel * 3u;
    const float luminance = input[rgb_index] * parameters.red_luminance
        + input[rgb_index + 1u] * parameters.green_luminance
        + input[rgb_index + 2u] * parameters.blue_luminance;
    guide[pixel] = log2(max(luminance, 5.9604645e-8f) / 0.18f);
}

// Full-detail Selective Tone uses a 48-pixel level-zero radius. Keep a separate bounded
// reflect-101 box pair instead of doubling the loop cost of Local Contrast's smaller
// replicated-border kernels.
kernel void warm_reflect_box_horizontal_v1(
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
    for (int offset = -64; offset <= 64; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const uint sample_x = warm_reflect101_coordinate(
            int(position.x) + offset,
            parameters.width
        );
        sum += input[position.y * parameters.width + sample_x];
    }
    output[position.y * parameters.width + position.x] = sum
        / float(radius * 2 + 1);
}

kernel void warm_reflect_box_vertical_v1(
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
    for (int offset = -64; offset <= 64; ++offset) {
        if (abs(offset) > radius) {
            continue;
        }
        const uint sample_y = warm_reflect101_coordinate(
            int(position.y) + offset,
            parameters.height
        );
        sum += input[sample_y * parameters.width + position.x];
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
    if (position.x >= parameters.width || position.y != 0u) {
        return;
    }
    float sum = 0.0f;
    float correction = 0.0f;
    const int radius = int(parameters.radius);
    for (int offset = -radius; offset <= radius; ++offset) {
        const uint sample_y = warm_clamp_coordinate(offset, parameters.height);
        warm_compensated_add(
            input[sample_y * parameters.width + position.x],
            sum,
            correction
        );
    }
    const float divisor = float(radius * 2 + 1);
    for (uint y = 0u; y < parameters.height; ++y) {
        output[y * parameters.width + position.x] = sum / divisor;
        if (y + 1u >= parameters.height) {
            continue;
        }
        const uint leaving_y = warm_clamp_coordinate(int(y) - radius, parameters.height);
        const uint entering_y = warm_clamp_coordinate(
            int(y) + radius + 1,
            parameters.height
        );
        warm_compensated_add(
            -input[leaving_y * parameters.width + position.x],
            sum,
            correction
        );
        warm_compensated_add(
            input[entering_y * parameters.width + position.x],
            sum,
            correction
        );
    }
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

inline float warm_log2_one_plus_exp2(float value) {
    return value >= 0.0f
        ? value + log2(1.0f + exp2(-value))
        : log2(1.0f + exp2(value));
}

inline float warm_lower_ev_hinge(float value, float boundary, float softness) {
    return softness * warm_log2_one_plus_exp2((boundary - value) / softness);
}

inline float warm_upper_ev_hinge(float value, float boundary, float softness) {
    return softness * warm_log2_one_plus_exp2((value - boundary) / softness);
}

kernel void warm_selective_tone_apply_v1(
    device const float* input [[buffer(0)]],
    device const float* mask [[buffer(1)]],
    device float* output [[buffer(2)]],
    constant WarmSelectiveToneParameters& parameters [[buffer(3)]],
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
    float3 lab = xyz_to_oklab(multiply_rows(
        parameters.rgb_to_xyz_row_0,
        parameters.rgb_to_xyz_row_1,
        parameters.rgb_to_xyz_row_2,
        rgb
    ));
    float3 adjusted = rgb;
    if (lab.x > 0.0f && isfinite(lab.x)) {
        constexpr float endpoint_strength = 0.86f;
        constexpr float endpoint_boundary_ev = 1.45f;
        constexpr float endpoint_softness_ev = 0.55f;
        constexpr float recovery_strength = 0.76f;
        constexpr float shadow_boundary_ev = -0.15f;
        constexpr float highlight_boundary_ev = 0.75f;
        constexpr float recovery_softness_ev = 0.95f;
        const float mask_ev = mask[pixel];
        float adjusted_ev = mask_ev;
        adjusted_ev += endpoint_strength * parameters.blacks
            * warm_lower_ev_hinge(
                adjusted_ev,
                -endpoint_boundary_ev,
                endpoint_softness_ev
            );
        adjusted_ev += recovery_strength * parameters.shadows
            * warm_lower_ev_hinge(
                adjusted_ev,
                shadow_boundary_ev,
                recovery_softness_ev
            );
        adjusted_ev += recovery_strength * parameters.highlights
            * warm_upper_ev_hinge(
                adjusted_ev,
                highlight_boundary_ev,
                recovery_softness_ev
            );
        adjusted_ev += endpoint_strength * parameters.whites
            * warm_upper_ev_hinge(
                adjusted_ev,
                endpoint_boundary_ev,
                endpoint_softness_ev
            );
        const float lightness_gain = exp2((adjusted_ev - mask_ev) / 3.0f);
        if (lightness_gain > 0.0f && isfinite(lightness_gain)) {
            lab.x *= lightness_gain;
            adjusted = multiply_rows(
                parameters.xyz_to_rgb_row_0,
                parameters.xyz_to_rgb_row_1,
                parameters.xyz_to_rgb_row_2,
                oklab_to_xyz(lab)
            );
        }
    }
    output[rgb_index] = adjusted.x;
    output[rgb_index + 1u] = adjusted.y;
    output[rgb_index + 2u] = adjusted.z;
}

)METAL";

} // namespace shadow::image::detail

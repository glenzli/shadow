#pragma once

#include <string>
#include <string_view>

namespace shadow::image::detail {

// Runtime-compiled Metal is used by both the standalone executor and the session-resident
// warm-preview executor. Keep the adjustment ABI and interpreter in one source fragment so the
// two kernels cannot silently acquire different node-order or resource-boundary semantics.
inline constexpr std::string_view metal_adjustment_msl_common = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant uint parameter_abi_version = 2u;
constant uint plan_identity_version = 1u;
constant uint opcode_white_balance = 1u;
constant uint opcode_exposure = 2u;
constant uint opcode_contrast = 3u;
constant uint opcode_saturation = 4u;
constant uint opcode_oklab_lightness_curve = 5u;
constant uint opcode_color_grading = 6u;
constant uint opcode_lut_3d = 7u;
constant uint status_non_finite = 1u;
constant uint status_bad_abi = 2u;
constant uint status_bad_opcode = 4u;
constant uint status_bad_resource = 8u;

struct MetalAdjustmentInvocation {
    uint abi_version;
    uint plan_identity_version;
    uint width;
    uint height;
    uint input_row_floats;
    uint output_row_floats;
    uint step_count;
    uint curve_segment_count;
    uint lut_entry_count;
    uint reserved_0;
    uint reserved_1;
    uint reserved_2;
    float4 rgb_to_xyz_row_0;
    float4 rgb_to_xyz_row_1;
    float4 rgb_to_xyz_row_2;
    float4 xyz_to_rgb_row_0;
    float4 xyz_to_rgb_row_1;
    float4 xyz_to_rgb_row_2;
    float4 working_luminance;
};

struct MetalAdjustmentOp {
    uint opcode;
    uint source_node_index;
    uint resource_offset;
    uint resource_count;
    float4 parameter_0;
    float4 parameter_1;
    float4 parameter_2;
};

struct MetalCurveSegment {
    // x, y, derivative, padding.
    float4 left;
    float4 right;
};

struct MetalAdjustmentStatus {
    atomic_uint flags;
    atomic_uint earliest_step;
    uint reserved_0;
    uint reserved_1;
};

inline float signed_cbrt(float value) {
    if (value == 0.0f) {
        return 0.0f;
    }
    // MSL has no cbrt overload. Safe-mode pow preserves signed extended-gamut inputs without
    // opting into the native/fast namespace.
    return copysign(pow(abs(value), 1.0f / 3.0f), value);
}

inline float3 multiply_rows(float4 row_0, float4 row_1, float4 row_2, float3 value) {
    return float3(
        dot(row_0.xyz, value),
        dot(row_1.xyz, value),
        dot(row_2.xyz, value)
    );
}

inline float3 xyz_to_oklab(float3 xyz) {
    const float l = signed_cbrt(
        0.8190224379967030f * xyz.x + 0.3619062600528904f * xyz.y
            - 0.1288737815209879f * xyz.z
    );
    const float m = signed_cbrt(
        0.0329836539323885f * xyz.x + 0.9292868615863434f * xyz.y
            + 0.0361446663506424f * xyz.z
    );
    const float s = signed_cbrt(
        0.0481771893596242f * xyz.x + 0.2642395317527308f * xyz.y
            + 0.6335478284694309f * xyz.z
    );
    return float3(
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    );
}

inline float3 oklab_to_xyz(float3 lab) {
    const float l_root = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const float m_root = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const float s_root = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const float l = l_root * l_root * l_root;
    const float m = m_root * m_root * m_root;
    const float s = s_root * s_root * s_root;
    return float3(
        1.2268798758459240f * l - 0.5578149944602170f * m
            + 0.2813910456659646f * s,
        -0.0405757452148009f * l + 1.1122868032803173f * m
            - 0.0717110580655164f * s,
        -0.0763729366746600f * l - 0.4214933324022431f * m
            + 1.5869240198367816f * s
    );
}

inline float3 working_rgb_to_oklab(
    float3 rgb,
    constant MetalAdjustmentInvocation& invocation
) {
    return xyz_to_oklab(multiply_rows(
        invocation.rgb_to_xyz_row_0,
        invocation.rgb_to_xyz_row_1,
        invocation.rgb_to_xyz_row_2,
        rgb
    ));
}

inline float3 oklab_to_working_rgb(
    float3 lab,
    constant MetalAdjustmentInvocation& invocation
) {
    return multiply_rows(
        invocation.xyz_to_rgb_row_0,
        invocation.xyz_to_rgb_row_1,
        invocation.xyz_to_rgb_row_2,
        oklab_to_xyz(lab)
    );
}

inline void report_adjustment_failure(
    device MetalAdjustmentStatus& status,
    uint flag,
    uint step
) {
    atomic_fetch_or_explicit(&status.flags, flag, memory_order_relaxed);
    atomic_fetch_min_explicit(&status.earliest_step, step, memory_order_relaxed);
}

inline bool resource_range_is_valid(uint offset, uint count, uint available) {
    return count > 0u && offset <= available && count <= available - offset;
}

inline float evaluate_curve_segment(
    const MetalCurveSegment segment,
    float value
) {
    const float width = segment.right.x - segment.left.x;
    const float t = (value - segment.left.x) / width;
    const float t_squared = t * t;
    const float t_cubed = t_squared * t;
    const float h00 = 2.0f * t_cubed - 3.0f * t_squared + 1.0f;
    const float h10 = t_cubed - 2.0f * t_squared + t;
    const float h01 = -2.0f * t_cubed + 3.0f * t_squared;
    const float h11 = t_cubed - t_squared;
    return h00 * segment.left.y + h10 * width * segment.left.z
        + h01 * segment.right.y + h11 * width * segment.right.z;
}

inline float evaluate_oklab_lightness_curve(
    device const MetalCurveSegment* curve_segments,
    const MetalAdjustmentOp operation,
    float value
) {
    // Host preparation duplicates the two endpoint knots in parameter_0/1. This keeps
    // extrapolation independent of any speculative side-table read.
    if (value <= operation.parameter_0.x) {
        return operation.parameter_0.y
            + (value - operation.parameter_0.x) * operation.parameter_0.z;
    }
    if (value >= operation.parameter_1.x) {
        return operation.parameter_1.y
            + (value - operation.parameter_1.x) * operation.parameter_1.z;
    }

    // Match std::upper_bound from the CPU oracle: a value exactly on an interior knot selects
    // the segment beginning at that knot.
    uint lower = 0u;
    uint upper = operation.resource_count;
    while (lower < upper) {
        const uint middle = lower + (upper - lower) / 2u;
        const MetalCurveSegment candidate =
            curve_segments[operation.resource_offset + middle];
        if (value < candidate.right.x) {
            upper = middle;
        } else {
            lower = middle + 1u;
        }
    }
    const uint segment_index = min(lower, operation.resource_count - 1u);
    return evaluate_curve_segment(
        curve_segments[operation.resource_offset + segment_index],
        value
    );
}

inline float adjustment_smoothstep(float lower, float upper, float value) {
    if (value <= lower) {
        return 0.0f;
    }
    if (value >= upper) {
        return 1.0f;
    }
    const float normalized = (value - lower) / (upper - lower);
    return normalized * normalized * (3.0f - 2.0f * normalized);
}

inline float3 apply_color_grading(
    float3 rgb,
    const MetalAdjustmentOp operation,
    constant MetalAdjustmentInvocation& invocation
) {
    const float luma = dot(invocation.working_luminance.xyz, rgb);
    const float nonnegative_luma = max(0.0f, luma);
    const float normalized = nonnegative_luma / (nonnegative_luma + 0.18f);
    const float center = operation.parameter_0.w;
    const float width = operation.parameter_1.w;
    float shadow_weight =
        1.0f - adjustment_smoothstep(center - width, center + width, normalized);
    float highlight_weight =
        adjustment_smoothstep(center - width, center + width, normalized);
    float midtone_weight = 1.0f - abs(normalized - center) / max(0.12f, 0.5f + width);
    midtone_weight = clamp(midtone_weight, 0.0f, 1.0f);
    const float total = shadow_weight + midtone_weight + highlight_weight;
    shadow_weight /= total;
    midtone_weight /= total;
    highlight_weight /= total;

    float3 lab = working_rgb_to_oklab(rgb, invocation);
    // Host preparation folds the wheel's scale and trigonometry into (delta-a, delta-b,
    // delta-L), eliminating a second hue interpretation in the shader.
    lab.x += operation.parameter_0.z * shadow_weight;
    lab.y += operation.parameter_0.x * shadow_weight;
    lab.z += operation.parameter_0.y * shadow_weight;
    lab.x += operation.parameter_1.z * midtone_weight;
    lab.y += operation.parameter_1.x * midtone_weight;
    lab.z += operation.parameter_1.y * midtone_weight;
    lab.x += operation.parameter_2.z * highlight_weight;
    lab.y += operation.parameter_2.x * highlight_weight;
    lab.z += operation.parameter_2.y * highlight_weight;
    return oklab_to_working_rgb(lab, invocation);
}

inline uint lut_index(uint size, uint red, uint green, uint blue) {
    // .cube's canonical order is red-fastest, then green, then blue.
    return (blue * size + green) * size + red;
}

inline float3 convex_lerp(float3 left, float3 right, float amount) {
    // Avoid (right - left) overflowing for valid, opposite-sign extended-range samples.
    return left * (1.0f - amount) + right * amount;
}

inline float3 sample_lut_3d(
    device const float4* lut_entries,
    const MetalAdjustmentOp operation,
    float3 input
) {
    const uint size = operation.resource_count;
    const float3 domain_min = operation.parameter_0.yzw;
    const float3 domain_max = operation.parameter_1.xyz;
    const float3 normalized = clamp(
        (input - domain_min) / (domain_max - domain_min),
        0.0f,
        1.0f
    );
    const float3 position = normalized * float(size - 1u);
    const uint3 lower = uint3(floor(position));
    const uint3 upper = min(lower + uint3(1u), uint3(size - 1u));
    const float3 fraction = position - float3(lower);
    const uint offset = operation.resource_offset;

    const float3 c000 =
        lut_entries[offset + lut_index(size, lower.x, lower.y, lower.z)].xyz;
    const float3 c100 =
        lut_entries[offset + lut_index(size, upper.x, lower.y, lower.z)].xyz;
    const float3 c010 =
        lut_entries[offset + lut_index(size, lower.x, upper.y, lower.z)].xyz;
    const float3 c110 =
        lut_entries[offset + lut_index(size, upper.x, upper.y, lower.z)].xyz;
    const float3 c001 =
        lut_entries[offset + lut_index(size, lower.x, lower.y, upper.z)].xyz;
    const float3 c101 =
        lut_entries[offset + lut_index(size, upper.x, lower.y, upper.z)].xyz;
    const float3 c011 =
        lut_entries[offset + lut_index(size, lower.x, upper.y, upper.z)].xyz;
    const float3 c111 =
        lut_entries[offset + lut_index(size, upper.x, upper.y, upper.z)].xyz;

    // Preserve the CPU oracle's interpolation order: red, then green, then blue.
    const float3 c00 = convex_lerp(c000, c100, fraction.x);
    const float3 c10 = convex_lerp(c010, c110, fraction.x);
    const float3 c01 = convex_lerp(c001, c101, fraction.x);
    const float3 c11 = convex_lerp(c011, c111, fraction.x);
    const float3 c0 = convex_lerp(c00, c10, fraction.y);
    const float3 c1 = convex_lerp(c01, c11, fraction.y);
    return convex_lerp(c0, c1, fraction.z);
}

inline bool execute_adjustment_program(
    thread float3& rgb,
    device const MetalAdjustmentOp* operations,
    device const MetalCurveSegment* curve_segments,
    device const float4* lut_entries,
    constant MetalAdjustmentInvocation& invocation,
    device MetalAdjustmentStatus& status
) {
    for (uint step = 0u; step < invocation.step_count; ++step) {
        const MetalAdjustmentOp operation = operations[step];
        switch (operation.opcode) {
        case opcode_white_balance:
            rgb = multiply_rows(
                operation.parameter_0,
                operation.parameter_1,
                operation.parameter_2,
                rgb
            );
            break;
        case opcode_exposure:
            rgb *= operation.parameter_0.x;
            break;
        case opcode_contrast: {
            float3 lab = working_rgb_to_oklab(rgb, invocation);
            if (lab.x > 0.0f && isfinite(lab.x)) {
                const float pivot = operation.parameter_0.x;
                if (operation.parameter_0.z != 0.0f) {
                    lab.x = pivot;
                } else {
                    const float normalized = lab.x / (lab.x + pivot);
                    const float amount = operation.parameter_0.y;
                    const float shaped = normalized
                        + amount * 2.0f * normalized * (1.0f - normalized)
                            * (2.0f * normalized - 1.0f);
                    const float bounded = clamp(shaped, 1.0e-7f, 1.0f - 1.0e-7f);
                    lab.x = pivot * bounded / (1.0f - bounded);
                }
                rgb = oklab_to_working_rgb(lab, invocation);
            }
            break;
        }
        case opcode_saturation:
            // Match the CPU oracle's exact neutral-axis bypass before any matrix round trip.
            if (!(rgb.x == rgb.y && rgb.y == rgb.z)) {
                float3 lab = working_rgb_to_oklab(rgb, invocation);
                lab.yz *= operation.parameter_0.x;
                rgb = oklab_to_working_rgb(lab, invocation);
            }
            break;
        case opcode_oklab_lightness_curve:
            if (!resource_range_is_valid(
                    operation.resource_offset,
                    operation.resource_count,
                    invocation.curve_segment_count
                )) {
                report_adjustment_failure(status, status_bad_resource, step);
                return false;
            } else {
                float3 lab = working_rgb_to_oklab(rgb, invocation);
                lab.x = evaluate_oklab_lightness_curve(
                    curve_segments,
                    operation,
                    lab.x
                );
                rgb = oklab_to_working_rgb(lab, invocation);
            }
            break;
        case opcode_color_grading:
            rgb = apply_color_grading(rgb, operation, invocation);
            break;
        case opcode_lut_3d: {
            const uint size = operation.resource_count;
            // The public parser admits 2..65. Keep the shader self-defending before cubing size,
            // then prove the red-fastest subrange is contained in the bound side table.
            if (size < 2u || size > 65u) {
                report_adjustment_failure(status, status_bad_resource, step);
                return false;
            }
            const uint entry_count = size * size * size;
            const float3 domain_min = operation.parameter_0.yzw;
            const float3 domain_max = operation.parameter_1.xyz;
            if (!resource_range_is_valid(
                    operation.resource_offset,
                    entry_count,
                    invocation.lut_entry_count
                )
                || !all(isfinite(domain_min))
                || !all(isfinite(domain_max))
                || !all(domain_min < domain_max)
                || !isfinite(operation.parameter_0.x)
                || operation.parameter_0.x < 0.0f
                || operation.parameter_0.x > 1.0f) {
                report_adjustment_failure(status, status_bad_resource, step);
                return false;
            }
            const float3 sampled = sample_lut_3d(lut_entries, operation, rgb);
            rgb = convex_lerp(rgb, sampled, operation.parameter_0.x);
            break;
        }
        default:
            report_adjustment_failure(status, status_bad_opcode, step);
            return false;
        }
        if (!all(isfinite(rgb))) {
            report_adjustment_failure(status, status_non_finite, step);
            return false;
        }
    }
    return true;
}
)METAL";

[[nodiscard]] inline std::string make_metal_adjustment_source(
    const std::string_view kernel_source
) {
    std::string result;
    result.reserve(metal_adjustment_msl_common.size() + kernel_source.size());
    result.append(metal_adjustment_msl_common);
    result.append(kernel_source);
    return result;
}

} // namespace shadow::image::detail

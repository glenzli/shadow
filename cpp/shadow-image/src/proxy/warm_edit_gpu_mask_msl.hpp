#pragma once

#include <string_view>

namespace shadow::image::detail {

// Local-mask evaluation is one runtime-compiled DSL responsibility. The capture kernel and layer
// blend deliberately share this evaluator so a selected active layer computes color/luminance
// conditions once, keeps the float coverage resident, and reuses it for blending.
inline constexpr std::string_view warm_mask_kernel_source = R"METAL(
kernel void warm_copy_rgb_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmLayerBlendParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint input_index =
        position.y * parameters.input_row_floats + position.x * 3u;
    const uint output_index = (position.y * parameters.width + position.x) * 3u;
    output[output_index] = input[input_index];
    output[output_index + 1u] = input[input_index + 1u];
    output[output_index + 2u] = input[input_index + 2u];
}

inline float warm_smootherstep(float value) {
    const float x = clamp(value, 0.0f, 1.0f);
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

inline float warm_brush_capsule_distance(
    float2 point,
    WarmBrushCapsule capsule,
    float2 scale
) {
    const float2 scaled_point = point * scale;
    const float2 start = float2(capsule.x0, capsule.y0) * scale;
    const float2 end = float2(capsule.x1, capsule.y1) * scale;
    const float2 direction = end - start;
    const float denominator = dot(direction, direction);
    if (denominator <= 2.220446049250313e-16f) {
        return length(scaled_point - end);
    }
    const float projection = clamp(
        dot(scaled_point - start, direction) / denominator,
        0.0f,
        1.0f
    );
    return length(scaled_point - (start + projection * direction));
}

inline float warm_layer_coverage(
    device const float* input,
    uint input_row_floats,
    constant WarmLayerBlendParameters& parameters,
    device const WarmBrushCapsule* brush_capsules,
    device const WarmBrushCellRange* brush_cells,
    device const uint* brush_references,
    uint2 position
) {
    const float x = (
        float(parameters.origin_x) + float(position.x) + 0.5f
    ) / float(parameters.full_width);
    const float y = (
        float(parameters.origin_y) + float(position.y) + 0.5f
    ) / float(parameters.full_height);
    const uint input_index =
        position.y * input_row_floats + position.x * 3u;
    const float3 source = float3(
        input[input_index],
        input[input_index + 1u],
        input[input_index + 2u]
    );
    float coverage = parameters.mask_kind == 6u ? 0.0f : 1.0f;
    if (parameters.mask_kind == 1u) {
        const float dx = parameters.x1 - parameters.x0;
        const float dy = parameters.y1 - parameters.y0;
        coverage = clamp(
            ((x - parameters.x0) * dx + (y - parameters.y0) * dy)
                / (dx * dx + dy * dy),
            0.0f,
            1.0f
        );
    } else if (parameters.mask_kind == 2u) {
        const float dx = (x - parameters.x0) / parameters.radius_x;
        const float dy = (y - parameters.y0) / parameters.radius_y;
        const float distance = sqrt(dx * dx + dy * dy);
        if (parameters.feather <= 0.0f) {
            coverage = distance <= 1.0f ? 1.0f : 0.0f;
        } else {
            const float inner = 1.0f - parameters.feather;
            coverage = 1.0f - warm_smootherstep(
                (distance - inner) / parameters.feather
            );
        }
    } else if (parameters.mask_kind == 3u) {
        if (parameters.brush_grid_columns == 0u
            || parameters.brush_grid_rows == 0u
            || parameters.brush_capsule_count == 0u) {
            return -1.0f;
        }
        const uint column = min(
            uint(x * float(parameters.brush_grid_columns)),
            parameters.brush_grid_columns - 1u
        );
        const uint row = min(
            uint(y * float(parameters.brush_grid_rows)),
            parameters.brush_grid_rows - 1u
        );
        const WarmBrushCellRange range =
            brush_cells[row * parameters.brush_grid_columns + column];
        if (range.offset > parameters.brush_reference_count
            || range.count > parameters.brush_reference_count - range.offset) {
            return -1.0f;
        }
        const float shorter_side =
            float(min(parameters.full_width, parameters.full_height));
        const float2 scale = float2(
            float(parameters.full_width) / shorter_side,
            float(parameters.full_height) / shorter_side
        );
        float distance = 3.402823466e+38f;
        for (uint candidate = 0u; candidate < range.count; ++candidate) {
            const uint capsule_index =
                brush_references[range.offset + candidate];
            if (capsule_index >= parameters.brush_capsule_count) {
                return -1.0f;
            }
            distance = min(
                distance,
                warm_brush_capsule_distance(
                    float2(x, y),
                    brush_capsules[capsule_index],
                    scale
                )
            );
        }
        const float inner =
            parameters.radius_x * (1.0f - parameters.feather);
        const float transition = max(
            parameters.radius_x - inner,
            2.220446049250313e-16f
        );
        coverage = parameters.feather <= 0.0f
            ? (distance <= parameters.radius_x ? 1.0f : 0.0f)
            : 1.0f - warm_smootherstep((distance - inner) / transition);
    } else if (parameters.mask_kind == 4u) {
        const float3 lab = xyz_to_oklab(multiply_rows(
            parameters.rgb_to_xyz_row_0,
            parameters.rgb_to_xyz_row_1,
            parameters.rgb_to_xyz_row_2,
            source
        ));
        const float lightness = clamp(lab.x, 0.0f, 1.0f);
        if (parameters.feather <= 0.0f) {
            coverage =
                lightness >= parameters.x0 && lightness <= parameters.x1
                ? 1.0f
                : 0.0f;
        } else {
            const float lower = warm_smootherstep(
                (lightness - (parameters.x0 - parameters.feather))
                    / parameters.feather
            );
            const float upper = 1.0f - warm_smootherstep(
                (lightness - parameters.x1) / parameters.feather
            );
            coverage = min(lower, upper);
        }
    } else if (parameters.mask_kind == 5u) {
        const float3 lab = xyz_to_oklab(multiply_rows(
            parameters.rgb_to_xyz_row_0,
            parameters.rgb_to_xyz_row_1,
            parameters.rgb_to_xyz_row_2,
            source
        ));
        const float chroma = length(lab.yz);
        const float relative_chroma =
            chroma / max(1.0e-6f, abs(lab.x));
        const float confidence =
            adjustment_smoothstep(0.002f, 0.02f, relative_chroma);
        const float hue =
            wrap_degrees(atan2(lab.z, lab.y) * (180.0f / adjustment_pi));
        coverage = confidence * perceptual_range_weight(
            float4(
                1.0f,
                parameters.x0 * 360.0f,
                parameters.x1 * 180.0f,
                parameters.feather
            ),
            hue
        );
    } else if (parameters.mask_kind > 6u) {
        return -1.0f;
    }
    if (parameters.invert != 0u) {
        coverage = 1.0f - coverage;
    }
    return clamp(coverage, 0.0f, 1.0f);
}

kernel void warm_mask_coverage_v1(
    device const float* input [[buffer(0)]],
    device float* linear_coverage [[buffer(1)]],
    device uchar* r8_coverage [[buffer(2)]],
    constant WarmLayerBlendParameters& parameters [[buffer(3)]],
    device MetalAdjustmentStatus& status [[buffer(4)]],
    device const WarmBrushCapsule* brush_capsules [[buffer(5)]],
    device const WarmBrushCellRange* brush_cells [[buffer(6)]],
    device const uint* brush_references [[buffer(7)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const float coverage = warm_layer_coverage(
        input,
        parameters.input_row_floats,
        parameters,
        brush_capsules,
        brush_cells,
        brush_references,
        position
    );
    if (!(coverage >= 0.0f) || !isfinite(coverage)) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        return;
    }
    const uint index = position.y * parameters.width + position.x;
    linear_coverage[index] = coverage;
    r8_coverage[index] =
        uchar(clamp(floor(coverage * 255.0f + 0.5f), 0.0f, 255.0f));
}

kernel void warm_layer_blend_v1(
    device const float* before [[buffer(0)]],
    device float* after [[buffer(1)]],
    constant WarmLayerBlendParameters& parameters [[buffer(2)]],
    device MetalAdjustmentStatus& status [[buffer(3)]],
    device const WarmBrushCapsule* brush_capsules [[buffer(4)]],
    device const WarmBrushCellRange* brush_cells [[buffer(5)]],
    device const uint* brush_references [[buffer(6)]],
    device const float* precomputed_coverage [[buffer(7)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint coverage_index =
        position.y * parameters.width + position.x;
    const float coverage = parameters.use_precomputed_coverage != 0u
        ? precomputed_coverage[coverage_index]
        : warm_layer_coverage(
            before,
            parameters.width * 3u,
            parameters,
            brush_capsules,
            brush_cells,
            brush_references,
            position
        );
    if (!(coverage >= 0.0f) || !isfinite(coverage)) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        return;
    }
    const uint index = coverage_index * 3u;
    const float3 source = float3(
        before[index],
        before[index + 1u],
        before[index + 2u]
    );
    const float alpha = parameters.opacity * coverage;
    const float3 adjusted = float3(
        after[index],
        after[index + 1u],
        after[index + 2u]
    );
    const float3 mixed =
        fma(float3(alpha), adjusted - source, source);
    if (!all(isfinite(mixed))) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    after[index] = mixed.x;
    after[index + 1u] = mixed.y;
    after[index + 2u] = mixed.z;
}

)METAL";

} // namespace shadow::image::detail

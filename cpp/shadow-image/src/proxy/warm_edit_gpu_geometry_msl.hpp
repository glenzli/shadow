#pragma once

#include <string_view>

namespace shadow::image::detail {

inline constexpr std::string_view warm_geometry_kernel_source = R"METAL(
struct WarmPhotoGeometryParameters {
    uint input_width;
    uint input_height;
    uint input_row_floats;
    uint reserved_0;
    uint source_tile_origin_x;
    uint source_tile_origin_y;
    uint source_crop_origin_x;
    uint source_crop_origin_y;
    uint source_crop_width;
    uint source_crop_height;
    uint output_canvas_width;
    uint output_canvas_height;
    uint output_origin_x;
    uint output_origin_y;
    uint output_width;
    uint output_height;
    uint quarter_turn;
    uint flip_horizontal;
    uint flip_vertical;
    uint reserved_1;
    float straighten_cosine;
    float straighten_sine;
    float reserved_2;
    float reserved_3;
};

struct WarmPhotoLiquifyParameters {
    uint source_width;
    uint source_height;
    uint grid_columns;
    uint grid_rows;
    float cell_width;
    float cell_height;
    uint stamp_count;
    uint cell_range_offset_words;
    uint reference_offset_words;
    uint reference_count;
    uint reserved_0;
    uint reserved_1;
};

struct WarmGeometryLookup {
    uint source_x0;
    uint source_y0;
    uint source_x1;
    uint source_y1;
    float fraction_x;
    float fraction_y;
    uint state;
    uint reserved;
};

inline float warm_liquify_smootherstep(float value) {
    const float bounded = clamp(value, 0.0f, 1.0f);
    return bounded * bounded * bounded
        * fma(bounded, fma(bounded, 6.0f, -15.0f), 10.0f);
}

inline float2 warm_inverse_liquify_coordinate(
    constant WarmPhotoLiquifyParameters& parameters,
    device const uint* words,
    device MetalAdjustmentStatus& status,
    float2 coordinate,
    thread bool& valid
) {
    if (parameters.stamp_count == 0u) {
        return coordinate;
    }
    if (parameters.source_width == 0u || parameters.source_height == 0u
        || parameters.grid_columns == 0u || parameters.grid_rows == 0u
        || !isfinite(parameters.cell_width) || parameters.cell_width <= 0.0f
        || !isfinite(parameters.cell_height) || parameters.cell_height <= 0.0f) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        valid = false;
        return coordinate;
    }
    const float2 bounded = clamp(
        coordinate,
        float2(0.0f),
        float2(
            float(parameters.source_width - 1u),
            float(parameters.source_height - 1u)
        )
    );
    const uint column = min(
        uint(bounded.x / parameters.cell_width),
        parameters.grid_columns - 1u
    );
    const uint row = min(
        uint(bounded.y / parameters.cell_height),
        parameters.grid_rows - 1u
    );
    const uint cell = row * parameters.grid_columns + column;
    const uint range_word =
        parameters.cell_range_offset_words + cell * 2u;
    const uint reference_offset = words[range_word];
    const uint reference_count = words[range_word + 1u];
    if (reference_offset > parameters.reference_count
        || reference_count > parameters.reference_count - reference_offset) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        valid = false;
        return coordinate;
    }
    float active_weight = 1.0f;
    float2 reconstructed = float2(0.0f);
    for (uint reference = 0u; reference < reference_count; ++reference) {
        const uint stamp_index = words[
            parameters.reference_offset_words + reference_offset + reference
        ];
        if (stamp_index >= parameters.stamp_count) {
            report_adjustment_failure(status, status_bad_resource, 0u);
            valid = false;
            return coordinate;
        }
        const uint stamp_word = stamp_index * 8u;
        const uint kind = words[stamp_word];
        const float2 center = float2(
            as_type<float>(words[stamp_word + 1u]),
            as_type<float>(words[stamp_word + 2u])
        );
        const float2 displacement = float2(
            as_type<float>(words[stamp_word + 3u]),
            as_type<float>(words[stamp_word + 4u])
        );
        const float reconstruction = as_type<float>(words[stamp_word + 5u]);
        const float radius = as_type<float>(words[stamp_word + 6u]);
        const float hardness = as_type<float>(words[stamp_word + 7u]);
        if (kind > 1u || !all(isfinite(center)) || !all(isfinite(displacement))
            || !isfinite(reconstruction) || reconstruction < 0.0f
            || reconstruction > 1.0f
            || !isfinite(radius) || radius <= 0.0f
            || !isfinite(hardness) || hardness < 0.0f || hardness > 1.0f) {
            report_adjustment_failure(status, status_bad_resource, 0u);
            valid = false;
            return coordinate;
        }
        const float distance = length(coordinate - center);
        if (distance >= radius) {
            continue;
        }
        const float normalized_distance = distance / radius;
        float weight = 1.0f;
        if (hardness < 1.0f && normalized_distance > hardness) {
            const float falloff_position =
                (normalized_distance - hardness) / (1.0f - hardness);
            weight = 1.0f - warm_liquify_smootherstep(falloff_position);
        }
        weight = clamp(weight, 0.0f, 1.0f);
        if (kind == 1u) {
            const float amount = clamp(reconstruction * weight, 0.0f, 1.0f);
            reconstructed += active_weight * amount * coordinate;
            active_weight *= 1.0f - amount;
        } else {
            coordinate -= displacement * weight;
        }
    }
    return reconstructed + active_weight * coordinate;
}

inline WarmGeometryLookup warm_geometry_lookup(
    constant WarmPhotoGeometryParameters& parameters,
    constant WarmPhotoLiquifyParameters& liquify,
    device const uint* liquify_words,
    device MetalAdjustmentStatus& status,
    uint2 position
) {
    WarmGeometryLookup lookup{};
    if (parameters.quarter_turn > 3u) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        lookup.state = 2u;
        return lookup;
    }

    const bool transposed = parameters.quarter_turn == 1u
        || parameters.quarter_turn == 3u;
    const float oriented_width = float(
        transposed ? parameters.source_crop_height : parameters.source_crop_width
    );
    const float oriented_height = float(
        transposed ? parameters.source_crop_width : parameters.source_crop_height
    );
    const float output_x = float(parameters.output_origin_x + position.x) + 0.5f
        - float(parameters.output_canvas_width) * 0.5f;
    const float output_y = float(parameters.output_origin_y + position.y) + 0.5f
        - float(parameters.output_canvas_height) * 0.5f;
    const float oriented_x = fma(
        parameters.straighten_cosine,
        output_x,
        parameters.straighten_sine * output_y
    ) + oriented_width * 0.5f;
    const float oriented_y = fma(
        -parameters.straighten_sine,
        output_x,
        parameters.straighten_cosine * output_y
    ) + oriented_height * 0.5f;

    float crop_x = 0.0f;
    float crop_y = 0.0f;
    switch (parameters.quarter_turn) {
    case 0u:
        crop_x = oriented_x;
        crop_y = oriented_y;
        break;
    case 1u:
        crop_x = oriented_y;
        crop_y = float(parameters.source_crop_height) - oriented_x;
        break;
    case 2u:
        crop_x = float(parameters.source_crop_width) - oriented_x;
        crop_y = float(parameters.source_crop_height) - oriented_y;
        break;
    default:
        crop_x = float(parameters.source_crop_width) - oriented_y;
        crop_y = oriented_x;
        break;
    }
    if (parameters.flip_horizontal != 0u) {
        crop_x = float(parameters.source_crop_width) - crop_x;
    }
    if (parameters.flip_vertical != 0u) {
        crop_y = float(parameters.source_crop_height) - crop_y;
    }
    float2 source = float2(
        float(parameters.source_crop_origin_x) + crop_x - 0.5f,
        float(parameters.source_crop_origin_y) + crop_y - 0.5f
    );
    const float crop_left = float(parameters.source_crop_origin_x);
    const float crop_top = float(parameters.source_crop_origin_y);
    const float crop_right = crop_left + float(parameters.source_crop_width - 1u);
    const float crop_bottom = crop_top + float(parameters.source_crop_height - 1u);
    if (source.x < crop_left || source.x > crop_right
        || source.y < crop_top || source.y > crop_bottom) {
        lookup.state = 1u;
        return lookup;
    }

    bool liquify_valid = true;
    source = warm_inverse_liquify_coordinate(
        liquify,
        liquify_words,
        status,
        source,
        liquify_valid
    );
    if (!liquify_valid) {
        lookup.state = 2u;
        return lookup;
    }
    const bool has_liquify = liquify.stamp_count > 0u;
    if (has_liquify) {
        source = clamp(
            source,
            float2(0.0f),
            float2(
                float(liquify.source_width - 1u),
                float(liquify.source_height - 1u)
            )
        );
    }
    const uint source_x0 = uint(floor(source.x));
    const uint source_y0 = uint(floor(source.y));
    lookup.fraction_x = source.x - float(source_x0);
    lookup.fraction_y = source.y - float(source_y0);
    const uint source_max_x = has_liquify
        ? liquify.source_width - 1u
        : parameters.source_crop_origin_x + parameters.source_crop_width - 1u;
    const uint source_max_y = has_liquify
        ? liquify.source_height - 1u
        : parameters.source_crop_origin_y + parameters.source_crop_height - 1u;
    const uint source_x1 = min(
        lookup.fraction_x == 0.0f ? source_x0 : source_x0 + 1u,
        source_max_x
    );
    const uint source_y1 = min(
        lookup.fraction_y == 0.0f ? source_y0 : source_y0 + 1u,
        source_max_y
    );
    if (source_x0 < parameters.source_tile_origin_x
        || source_y0 < parameters.source_tile_origin_y
        || source_x1 >= parameters.source_tile_origin_x + parameters.input_width
        || source_y1 >= parameters.source_tile_origin_y + parameters.input_height) {
        report_adjustment_failure(status, status_bad_resource, 0u);
        lookup.state = 2u;
        return lookup;
    }
    lookup.source_x0 = source_x0 - parameters.source_tile_origin_x;
    lookup.source_y0 = source_y0 - parameters.source_tile_origin_y;
    lookup.source_x1 = source_x1 - parameters.source_tile_origin_x;
    lookup.source_y1 = source_y1 - parameters.source_tile_origin_y;
    return lookup;
}

kernel void warm_photo_geometry_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmPhotoGeometryParameters& parameters [[buffer(2)]],
    device MetalAdjustmentStatus& status [[buffer(3)]],
    constant WarmPhotoLiquifyParameters& liquify [[buffer(4)]],
    device const uint* liquify_words [[buffer(5)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width || position.y >= parameters.output_height) {
        return;
    }
    const WarmGeometryLookup lookup =
        warm_geometry_lookup(parameters, liquify, liquify_words, status, position);
    const uint output_index =
        (position.y * parameters.output_width + position.x) * 3u;
    if (lookup.state == 1u) {
        output[output_index] = 0.0f;
        output[output_index + 1u] = 0.0f;
        output[output_index + 2u] = 0.0f;
        return;
    }
    if (lookup.state != 0u) {
        return;
    }
    const uint index_00 =
        lookup.source_y0 * parameters.input_row_floats + lookup.source_x0 * 3u;
    const uint index_10 =
        lookup.source_y0 * parameters.input_row_floats + lookup.source_x1 * 3u;
    const uint index_01 =
        lookup.source_y1 * parameters.input_row_floats + lookup.source_x0 * 3u;
    const uint index_11 =
        lookup.source_y1 * parameters.input_row_floats + lookup.source_x1 * 3u;
    const float3 top = mix(
        float3(input[index_00], input[index_00 + 1u], input[index_00 + 2u]),
        float3(input[index_10], input[index_10 + 1u], input[index_10 + 2u]),
        lookup.fraction_x
    );
    const float3 bottom = mix(
        float3(input[index_01], input[index_01 + 1u], input[index_01 + 2u]),
        float3(input[index_11], input[index_11 + 1u], input[index_11 + 2u]),
        lookup.fraction_x
    );
    const float3 sample = mix(top, bottom, lookup.fraction_y);
    if (!all(isfinite(sample))) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    output[output_index] = sample.x;
    output[output_index + 1u] = sample.y;
    output[output_index + 2u] = sample.z;
}

kernel void warm_mask_coverage_geometry_v1(
    device const float* input [[buffer(0)]],
    device uchar* output [[buffer(1)]],
    constant WarmPhotoGeometryParameters& parameters [[buffer(2)]],
    device MetalAdjustmentStatus& status [[buffer(3)]],
    constant WarmPhotoLiquifyParameters& liquify [[buffer(4)]],
    device const uint* liquify_words [[buffer(5)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width || position.y >= parameters.output_height) {
        return;
    }
    const WarmGeometryLookup lookup =
        warm_geometry_lookup(parameters, liquify, liquify_words, status, position);
    const uint output_index =
        position.y * parameters.output_width + position.x;
    if (lookup.state == 1u) {
        output[output_index] = uchar(0);
        return;
    }
    if (lookup.state != 0u) {
        return;
    }
    const uint index_00 =
        lookup.source_y0 * parameters.input_width + lookup.source_x0;
    const uint index_10 =
        lookup.source_y0 * parameters.input_width + lookup.source_x1;
    const uint index_01 =
        lookup.source_y1 * parameters.input_width + lookup.source_x0;
    const uint index_11 =
        lookup.source_y1 * parameters.input_width + lookup.source_x1;
    const float top = mix(input[index_00], input[index_10], lookup.fraction_x);
    const float bottom = mix(input[index_01], input[index_11], lookup.fraction_x);
    const float sample = mix(top, bottom, lookup.fraction_y);
    if (!isfinite(sample)) {
        report_adjustment_failure(status, status_non_finite, 0u);
        return;
    }
    output[output_index] =
        uchar(clamp(floor(sample * 255.0f + 0.5f), 0.0f, 255.0f));
}

)METAL";

} // namespace shadow::image::detail

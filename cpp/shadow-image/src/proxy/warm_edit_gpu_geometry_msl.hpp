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

inline WarmGeometryLookup warm_geometry_lookup(
    constant WarmPhotoGeometryParameters& parameters,
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
    const float source_x = float(parameters.source_crop_origin_x) + crop_x - 0.5f;
    const float source_y = float(parameters.source_crop_origin_y) + crop_y - 0.5f;
    const float crop_left = float(parameters.source_crop_origin_x);
    const float crop_top = float(parameters.source_crop_origin_y);
    const float crop_right = crop_left + float(parameters.source_crop_width - 1u);
    const float crop_bottom = crop_top + float(parameters.source_crop_height - 1u);
    if (source_x < crop_left || source_x > crop_right
        || source_y < crop_top || source_y > crop_bottom) {
        lookup.state = 1u;
        return lookup;
    }

    const uint source_x0 = uint(floor(source_x));
    const uint source_y0 = uint(floor(source_y));
    lookup.fraction_x = source_x - float(source_x0);
    lookup.fraction_y = source_y - float(source_y0);
    const uint source_x1 = min(
        lookup.fraction_x == 0.0f ? source_x0 : source_x0 + 1u,
        parameters.source_crop_origin_x + parameters.source_crop_width - 1u
    );
    const uint source_y1 = min(
        lookup.fraction_y == 0.0f ? source_y0 : source_y0 + 1u,
        parameters.source_crop_origin_y + parameters.source_crop_height - 1u
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
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width || position.y >= parameters.output_height) {
        return;
    }
    const WarmGeometryLookup lookup =
        warm_geometry_lookup(parameters, status, position);
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
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width || position.y >= parameters.output_height) {
        return;
    }
    const WarmGeometryLookup lookup =
        warm_geometry_lookup(parameters, status, position);
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

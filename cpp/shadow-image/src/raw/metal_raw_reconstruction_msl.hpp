#pragma once

namespace shadow::image::detail {

// Full-detail bilinear/edge-aware reconstruction and CFA-area preview kernels. Shared sampling,
// clipping, and highlight helpers are supplied by metal_raw_common_msl.hpp.
inline constexpr char metal_raw_reconstruction_source[] = R"METAL(
// High-quality detail reconstruction mirrors bayer_sampling.cpp: estimate green along the
// smoothest sensor direction, then interpolate red/blue as local colour differences. The
// bilinear sample remains the edge fallback; clipping is projected separately
// from the immutable RAW plane.
struct DirectionalGreenEstimate {
    float value;
    float gradient;
    bool valid;
};

inline DirectionalGreenEstimate try_directional_green(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint raw_x,
    const uint raw_y,
    const int dx,
    const int dy,
    const uint center_channel
) {
    const int left_x = int(raw_x) - dx;
    const int left_y = int(raw_y) - dy;
    const int right_x = int(raw_x) + dx;
    const int right_y = int(raw_y) + dy;
    const int far_left_x = int(raw_x) - 2 * dx;
    const int far_left_y = int(raw_y) - 2 * dy;
    const int far_right_x = int(raw_x) + 2 * dx;
    const int far_right_y = int(raw_y) + 2 * dy;
    if (left_x < 0 || left_y < 0 || right_x < 0 || right_y < 0
        || far_left_x < 0 || far_left_y < 0 || far_right_x < 0 || far_right_y < 0
        || left_x >= int(parameters.storage_width)
        || right_x >= int(parameters.storage_width)
        || far_left_x >= int(parameters.storage_width)
        || far_right_x >= int(parameters.storage_width)
        || left_y >= int(parameters.storage_height)
        || right_y >= int(parameters.storage_height)
        || far_left_y >= int(parameters.storage_height)
        || far_right_y >= int(parameters.storage_height)) {
        return DirectionalGreenEstimate{0.0f, 0.0f, false};
    }
    if (parameters.cfa_channels[cfa_site(uint(left_x), uint(left_y))] != 1u
        || parameters.cfa_channels[cfa_site(uint(right_x), uint(right_y))] != 1u
        || parameters.cfa_channels[cfa_site(uint(far_left_x), uint(far_left_y))]
            != center_channel
        || parameters.cfa_channels[cfa_site(uint(far_right_x), uint(far_right_y))]
            != center_channel) {
        return DirectionalGreenEstimate{0.0f, 0.0f, false};
    }
    const float left =
        normalized_sample(samples, parameters, uint(left_x), uint(left_y));
    const float right =
        normalized_sample(samples, parameters, uint(right_x), uint(right_y));
    const float far_left =
        normalized_sample(samples, parameters, uint(far_left_x), uint(far_left_y));
    const float far_right =
        normalized_sample(samples, parameters, uint(far_right_x), uint(far_right_y));
    const float center = normalized_sample(samples, parameters, raw_x, raw_y);
    const float chroma_laplacian = 2.0f * center - far_left - far_right;
    return DirectionalGreenEstimate{
        0.5f * (left + right) + 0.25f * chroma_laplacian,
        fabs(left - right) + fabs(chroma_laplacian),
        true,
    };
}

inline DirectionalGreenEstimate directional_green_estimate(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint raw_x,
    const uint raw_y
) {
    const uint center_channel = parameters.cfa_channels[cfa_site(raw_x, raw_y)];
    if (center_channel == 1u) {
        return DirectionalGreenEstimate{
            normalized_sample(samples, parameters, raw_x, raw_y),
            0.0f,
            true,
        };
    }
    if (center_channel != 0u && center_channel != 2u) {
        return DirectionalGreenEstimate{0.0f, 0.0f, false};
    }
    const DirectionalGreenEstimate horizontal = try_directional_green(
        samples,
        parameters,
        raw_x,
        raw_y,
        1,
        0,
        center_channel
    );
    const DirectionalGreenEstimate vertical = try_directional_green(
        samples,
        parameters,
        raw_x,
        raw_y,
        0,
        1,
        center_channel
    );
    if (horizontal.valid && vertical.valid) {
        constexpr float epsilon = 1.0e-5f;
        const float horizontal_weight = 1.0f / (epsilon + horizontal.gradient);
        const float vertical_weight = 1.0f / (epsilon + vertical.gradient);
        return DirectionalGreenEstimate{
            (horizontal.value * horizontal_weight + vertical.value * vertical_weight)
                / (horizontal_weight + vertical_weight),
            0.0f,
            true,
        };
    }
    if (horizontal.valid) {
        return horizontal;
    }
    return vertical;
}

inline float reconstruct_colour_difference(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint raw_x,
    const uint raw_y,
    const uint target_channel,
    const float center_green,
    const float fallback
) {
    const uint center_channel = parameters.cfa_channels[cfa_site(raw_x, raw_y)];
    if (center_channel == target_channel) {
        return normalized_sample(samples, parameters, raw_x, raw_y);
    }
    float weighted_sum = 0.0f;
    float total_weight = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        const int candidate_y = int(raw_y) + dy;
        if (candidate_y < 0 || candidate_y >= int(parameters.storage_height)) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            const int candidate_x = int(raw_x) + dx;
            if (candidate_x < 0 || candidate_x >= int(parameters.storage_width)) {
                continue;
            }
            const uint x = uint(candidate_x);
            const uint y = uint(candidate_y);
            if (parameters.cfa_channels[cfa_site(x, y)] != target_channel) {
                continue;
            }
            const DirectionalGreenEstimate neighbour_green =
                directional_green_estimate(samples, parameters, x, y);
            if (!neighbour_green.valid) {
                continue;
            }
            const float weight = dx == 0 || dy == 0 ? 1.0f : 0.7071067811865476f;
            weighted_sum += weight * (
                normalized_sample(samples, parameters, x, y)
                + center_green - neighbour_green.value
            );
            total_weight += weight;
        }
    }
    return total_weight > 0.0f ? weighted_sum / total_weight : fallback;
}

inline CameraRgbSample edge_aware_camera_rgb_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint raw_x,
    const uint raw_y
) {
    CameraRgbSample result = camera_rgb_at(samples, parameters, raw_x, raw_y);
    const DirectionalGreenEstimate green =
        directional_green_estimate(samples, parameters, raw_x, raw_y);
    if (!green.valid) {
        return result;
    }
    result.values.y = green.value;
    result.values.x = reconstruct_colour_difference(
        samples,
        parameters,
        raw_x,
        raw_y,
        0u,
        green.value,
        result.values.x
    );
    result.values.z = reconstruct_colour_difference(
        samples,
        parameters,
        raw_x,
        raw_y,
        2u,
        green.value,
        result.values.z
    );
    return result;
}

inline float3 develop_bayer_scene_linear_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint output_x,
    const uint output_y
) {
    uint source_x = output_x;
    uint source_y = output_y;
    switch (parameters.orientation) {
    case 3:
        source_x = parameters.reconstruction_width - 1u - output_x;
        source_y = parameters.reconstruction_height - 1u - output_y;
        break;
    case 5:
        source_x = parameters.reconstruction_width - 1u - output_y;
        source_y = output_x;
        break;
    case 6:
        source_x = output_y;
        source_y = parameters.reconstruction_height - 1u - output_x;
        break;
    default:
        break;
    }

    const uint raw_x = parameters.margin_left + source_x;
    const uint raw_y = parameters.margin_top + source_y;
    CameraRgbSample camera = parameters.reconstruction_quality == 2u
        ? edge_aware_camera_rgb_at(samples, parameters, raw_x, raw_y)
        : camera_rgb_at(samples, parameters, raw_x, raw_y);
    camera = reconstruct_cfa_highlights(camera, parameters);
    const float3 camera_values = camera.values;
    const float red =
        parameters.camera_to_linear_srgb[0] * camera_values.x
        + parameters.camera_to_linear_srgb[1] * camera_values.y
        + parameters.camera_to_linear_srgb[2] * camera_values.z;
    const float green =
        parameters.camera_to_linear_srgb[3] * camera_values.x
        + parameters.camera_to_linear_srgb[4] * camera_values.y
        + parameters.camera_to_linear_srgb[5] * camera_values.z;
    const float blue =
        parameters.camera_to_linear_srgb[6] * camera_values.x
        + parameters.camera_to_linear_srgb[7] * camera_values.y
        + parameters.camera_to_linear_srgb[8] * camera_values.z;
    return float3(red, green, blue);
}

kernel void develop_bayer_full(
    device const ushort* samples [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant RawDevelopmentParameters& parameters [[buffer(2)]],
    device const ushort* clipping_source [[buffer(3)]],
    device uchar* clipping_output [[buffer(4)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width
        || position.y >= parameters.output_tile_height) {
        return;
    }

    const uint output_x = position.x;
    const uint output_y = parameters.output_row_offset + position.y;
    if (parameters.project_sensor_clipping != 0u) {
        clipping_output[position.y * parameters.output_width + output_x] =
            sensor_clipping_flags(clipping_source, parameters, output_x, output_y);
    }
    const float3 scene_linear =
        develop_bayer_scene_linear_at(samples, parameters, output_x, output_y);
    const uint output_index =
        (position.y * parameters.output_width + output_x) * 3u;
    output[output_index] = scene_linear.x;
    output[output_index + 1u] = scene_linear.y;
    output[output_index + 2u] = scene_linear.z;
}

// Resident full-detail execution keeps the denoised CFA on the device and asks for bounded
// oriented output rectangles. The origin is explicit and the destination is local, so repeated
// viewport requests never reinterpret a tile as a miniature full image.
kernel void develop_bayer_resident_region(
    device const ushort* samples [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant RawDevelopmentParameters& parameters [[buffer(2)]],
    constant uint2& output_origin [[buffer(3)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width || position.y >= parameters.output_height) {
        return;
    }
    const uint output_x = output_origin.x + position.x;
    const uint output_y = output_origin.y + position.y;
    const bool transposed = parameters.orientation == 5 || parameters.orientation == 6;
    const uint full_width = transposed
        ? parameters.reconstruction_height : parameters.reconstruction_width;
    const uint full_height = transposed
        ? parameters.reconstruction_width : parameters.reconstruction_height;
    if (output_x >= full_width || output_y >= full_height) {
        return;
    }
    const float3 scene_linear =
        develop_bayer_scene_linear_at(samples, parameters, output_x, output_y);
    const uint output_index = (position.y * parameters.output_width + position.x) * 3u;
    output[output_index] = scene_linear.x;
    output[output_index + 1u] = scene_linear.y;
    output[output_index + 2u] = scene_linear.z;
}

// Preview pixels integrate their complete active-sensor footprint per CFA colour before the
// camera matrix. This is deliberately not a quick resized full development: that would alias
// Bayer phase into colour noise at fit-to-window scale. The CPU path uses the same footprint
// definition with double accumulation; Metal keeps the interactive path in f32.
kernel void develop_bayer_area_preview(
    device const ushort* samples [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant RawDevelopmentParameters& parameters [[buffer(2)]],
    device const ushort* clipping_source [[buffer(3)]],
    device uchar* clipping_output [[buffer(4)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width
        || position.y >= parameters.output_tile_height) {
        return;
    }

    const uint output_x = position.x;
    const uint output_y = parameters.output_row_offset + position.y;
    uint source_x = output_x;
    uint source_y = output_y;
    switch (parameters.orientation) {
    case 3:
        source_x = parameters.reconstruction_width - 1u - output_x;
        source_y = parameters.reconstruction_height - 1u - output_y;
        break;
    case 5:
        source_x = parameters.reconstruction_width - 1u - output_y;
        source_y = output_x;
        break;
    case 6:
        source_x = output_y;
        source_y = parameters.reconstruction_height - 1u - output_x;
        break;
    default:
        break;
    }

    const float scale_x = float(parameters.active_width) / float(parameters.reconstruction_width);
    const float scale_y = float(parameters.active_height) / float(parameters.reconstruction_height);
    const float active_left = float(parameters.margin_left);
    const float active_top = float(parameters.margin_top);
    const float active_right = active_left + float(parameters.active_width);
    const float active_bottom = active_top + float(parameters.active_height);
    const float source_left = active_left + float(source_x) * scale_x;
    const float source_right = min(active_right, active_left + float(source_x + 1u) * scale_x);
    const float source_top = active_top + float(source_y) * scale_y;
    const float source_bottom = min(active_bottom, active_top + float(source_y + 1u) * scale_y);
    const uint first_source_x = uint(floor(max(active_left, source_left)));
    const uint last_source_x = min(
        parameters.margin_left + parameters.active_width,
        uint(ceil(source_right))
    );
    const uint first_source_y = uint(floor(max(active_top, source_top)));
    const uint last_source_y = min(
        parameters.margin_top + parameters.active_height,
        uint(ceil(source_bottom))
    );
    float totals[3] = {0.0f, 0.0f, 0.0f};
    float weights[3] = {0.0f, 0.0f, 0.0f};
    float risk_maximums[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float risk_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint raw_y = first_source_y; raw_y < last_source_y; ++raw_y) {
        const float overlap_y = max(
            0.0f,
            min(source_bottom, float(raw_y + 1u)) - max(source_top, float(raw_y))
        );
        for (uint raw_x = first_source_x; raw_x < last_source_x; ++raw_x) {
            const float overlap_x = max(
                0.0f,
                min(source_right, float(raw_x + 1u)) - max(source_left, float(raw_x))
            );
            const uint channel = parameters.cfa_channels[cfa_site(raw_x, raw_y)];
            const float weight = overlap_x * overlap_y;
            const float normalized = normalized_sample(samples, parameters, raw_x, raw_y);
            totals[channel] += normalized * weight;
            weights[channel] += weight;
            const uint site = cfa_site(raw_x, raw_y);
            risk_maximums[site] = max(
                risk_maximums[site],
                cfa_highlight_risk(normalized_sensor_sample(samples, parameters, raw_x, raw_y))
            );
            risk_weights[site] += weight;
        }
    }
    // An active footprint always contains each CFA colour for supported previews. Preserve the
    // CPU fallback nonetheless so an edge rounding quirk cannot turn an unusual crop into NaN.
    const uint center_x = min(
        parameters.storage_width - 1u,
        uint((source_left + source_right) * 0.5f)
    );
    const uint center_y = min(
        parameters.storage_height - 1u,
        uint((source_top + source_bottom) * 0.5f)
    );
    CameraRgbSample camera = (
        weights[0] <= 0.0f || weights[1] <= 0.0f || weights[2] <= 0.0f
        || risk_weights[0] <= 0.0f || risk_weights[1] <= 0.0f
        || risk_weights[2] <= 0.0f || risk_weights[3] <= 0.0f
    ) ? camera_rgb_at(samples, parameters, center_x, center_y) : CameraRgbSample{
        float3(
            totals[0] / weights[0],
            totals[1] / weights[1],
            totals[2] / weights[2]
        ),
        float4(
            risk_maximums[0],
            risk_maximums[1],
            risk_maximums[2],
            risk_maximums[3]
        )
    };
    camera = reconstruct_cfa_highlights(camera, parameters);
    if (parameters.project_sensor_clipping != 0u) {
        clipping_output[position.y * parameters.output_width + output_x] =
            sensor_clipping_flags(clipping_source, parameters, output_x, output_y);
    }
    const float3 camera_values = camera.values;
    const float red =
        parameters.camera_to_linear_srgb[0] * camera_values.x
        + parameters.camera_to_linear_srgb[1] * camera_values.y
        + parameters.camera_to_linear_srgb[2] * camera_values.z;
    const float green =
        parameters.camera_to_linear_srgb[3] * camera_values.x
        + parameters.camera_to_linear_srgb[4] * camera_values.y
        + parameters.camera_to_linear_srgb[5] * camera_values.z;
    const float blue =
        parameters.camera_to_linear_srgb[6] * camera_values.x
        + parameters.camera_to_linear_srgb[7] * camera_values.y
        + parameters.camera_to_linear_srgb[8] * camera_values.z;
    const float3 scene_linear = float3(red, green, blue);
    const uint output_index = (position.y * parameters.output_width + output_x) * 3u;
    output[output_index] = scene_linear.x;
    output[output_index + 1u] = scene_linear.y;
    output[output_index + 2u] = scene_linear.z;
}

)METAL";

} // namespace shadow::image::detail

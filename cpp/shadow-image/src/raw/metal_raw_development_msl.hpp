#pragma once

namespace shadow::image::detail {

// Metal owns both native-size reconstruction and CFA-aware area previews. CPU remains the exact
// fallback when a device cannot satisfy the request or the host explicitly selects it.
inline constexpr char metal_raw_kernel_source[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct RawDevelopmentParameters {
    uint storage_width;
    uint storage_height;
    uint active_width;
    uint active_height;
    uint margin_left;
    uint margin_top;
    uint output_width;
    uint output_height;
    uint reconstruction_width;
    uint reconstruction_height;
    int orientation;
    uint output_row_offset;
    uint output_tile_height;
    uint neutralize_sensor_highlights;
    uint project_sensor_clipping;
    uint reconstruction_quality;
    uint cfa_channels[4];
    float black_levels[4];
    float white_minus_black[4];
    float camera_to_linear_srgb[9];
};

inline uint cfa_site(uint x, uint y) {
    return ((y & 1u) * 2u) + (x & 1u);
}

inline float normalized_sample(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint x,
    uint y
) {
    const uint site = cfa_site(x, y);
    const uint sample_index = y * parameters.storage_width + x;
    return (float(samples[sample_index]) - parameters.black_levels[site])
        / parameters.white_minus_black[site];
}

inline float sensor_clip_evidence(const float normalized) {
    return clamp((normalized - 0.98f) * 50.0f, 0.0f, 1.0f);
}

inline uint clipping_target_bin_begin(
    const uint target_coordinate,
    const uint source_extent,
    const uint target_extent
) {
    const ulong numerator = ulong(target_coordinate) * ulong(source_extent);
    return uint((numerator + ulong(target_extent) - 1ul) / ulong(target_extent));
}

inline uint clipping_target_bin_end(
    const uint target_coordinate,
    const uint source_extent,
    const uint target_extent
) {
    const ulong numerator = ulong(target_coordinate + 1u) * ulong(source_extent);
    return uint((numerator + ulong(target_extent) - 1ul) / ulong(target_extent));
}

inline uint2 clipping_active_coordinate(
    constant RawDevelopmentParameters& parameters,
    const uint oriented_x,
    const uint oriented_y
) {
    switch (parameters.orientation) {
    case 3:
        return uint2(
            parameters.active_width - 1u - oriented_x,
            parameters.active_height - 1u - oriented_y
        );
    case 5:
        return uint2(parameters.active_width - 1u - oriented_y, oriented_x);
    case 6:
        return uint2(oriented_y, parameters.active_height - 1u - oriented_x);
    default:
        return uint2(oriented_x, oriented_y);
    }
}

inline uchar sensor_clipping_flags(
    device const ushort* original_samples,
    constant RawDevelopmentParameters& parameters,
    const uint output_x,
    const uint output_y
) {
    const bool transposed = parameters.orientation == 5 || parameters.orientation == 6;
    const uint oriented_width = transposed ? parameters.active_height : parameters.active_width;
    const uint oriented_height = transposed ? parameters.active_width : parameters.active_height;
    if (parameters.output_width == oriented_width
        && parameters.output_height == oriented_height) {
        const uint2 active = clipping_active_coordinate(parameters, output_x, output_y);
        const uint raw_x = parameters.margin_left + active.x;
        const uint raw_y = parameters.margin_top + active.y;
        const uint site = cfa_site(raw_x, raw_y);
        const ushort sample = original_samples[raw_y * parameters.storage_width + raw_x];
        uchar flags = 0u;
        if (float(sample) <= parameters.black_levels[site]) {
            flags |= 2u;
        }
        if (float(sample)
            >= parameters.black_levels[site] + parameters.white_minus_black[site]) {
            flags |= 1u;
        }
        return flags;
    }
    const uint oriented_x_begin =
        clipping_target_bin_begin(output_x, oriented_width, parameters.output_width);
    const uint oriented_x_end =
        clipping_target_bin_end(output_x, oriented_width, parameters.output_width);
    const uint oriented_y_begin =
        clipping_target_bin_begin(output_y, oriented_height, parameters.output_height);
    const uint oriented_y_end =
        clipping_target_bin_end(output_y, oriented_height, parameters.output_height);
    bool observed = false;
    bool all_shadow = true;
    bool any_highlight = false;
    for (uint oriented_y = oriented_y_begin; oriented_y < oriented_y_end; ++oriented_y) {
        for (uint oriented_x = oriented_x_begin; oriented_x < oriented_x_end; ++oriented_x) {
            const uint2 active =
                clipping_active_coordinate(parameters, oriented_x, oriented_y);
            const uint raw_x = parameters.margin_left + active.x;
            const uint raw_y = parameters.margin_top + active.y;
            const uint site = cfa_site(raw_x, raw_y);
            const ushort sample = original_samples[raw_y * parameters.storage_width + raw_x];
            observed = true;
            all_shadow = all_shadow && float(sample) <= parameters.black_levels[site];
            any_highlight = any_highlight
                || float(sample)
                    >= parameters.black_levels[site] + parameters.white_minus_black[site];
        }
    }
    uchar flags = 0u;
    if (observed && all_shadow) {
        flags |= 2u;
    }
    if (any_highlight) {
        flags |= 1u;
    }
    return flags;
}

struct CameraRgbSample {
    float3 values;
    float3 sensor_clip_coverage;
};

inline CameraRgbSample camera_rgb_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint raw_x,
    uint raw_y
) {
    float totals[3] = {0.0f, 0.0f, 0.0f};
    float clipped_totals[3] = {0.0f, 0.0f, 0.0f};
    uint counts[3] = {0u, 0u, 0u};
    for (int dy = -1; dy <= 1; ++dy) {
        const int candidate_y = int(raw_y) + dy;
        if (candidate_y < 0 || candidate_y >= int(parameters.storage_height)) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            const int candidate_x = int(raw_x) + dx;
            if (candidate_x < 0 || candidate_x >= int(parameters.storage_width)) {
                continue;
            }
            const uint x = uint(candidate_x);
            const uint y = uint(candidate_y);
            const uint channel = parameters.cfa_channels[cfa_site(x, y)];
            const float normalized = normalized_sample(samples, parameters, x, y);
            totals[channel] += normalized;
            clipped_totals[channel] += sensor_clip_evidence(normalized);
            counts[channel] += 1u;
        }
    }
    return CameraRgbSample{
        float3(
            totals[0] / float(counts[0]),
            totals[1] / float(counts[1]),
            totals[2] / float(counts[2])
        ),
        float3(
            clipped_totals[0] / float(counts[0]),
            clipped_totals[1] / float(counts[1]),
            clipped_totals[2] / float(counts[2])
        )
    };
}

inline float smoothstep_scalar(const float edge0, const float edge1, const float value) {
    const float normalized = clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return normalized * normalized * (3.0f - 2.0f * normalized);
}

inline float3 neutralize_sensor_clipped_highlight(
    const float3 scene_linear,
    const CameraRgbSample camera
) {
    const float3 sensor_clip_coverage = camera.sensor_clip_coverage;
    const float lowest = min(
        sensor_clip_coverage.x,
        min(sensor_clip_coverage.y, sensor_clip_coverage.z)
    );
    const float highest = max(
        sensor_clip_coverage.x,
        max(sensor_clip_coverage.y, sensor_clip_coverage.z)
    );
    const float second_highest = sensor_clip_coverage.x + sensor_clip_coverage.y
        + sensor_clip_coverage.z - lowest - highest;
    const float camera_lowest = min(
        camera.values.x,
        min(camera.values.y, camera.values.z)
    );
    const float camera_highest = max(
        camera.values.x,
        max(camera.values.y, camera.values.z)
    );
    const float camera_second_highest = camera.values.x + camera.values.y + camera.values.z
        - camera_lowest - camera_highest;
    const float multi_channel_clip = smoothstep_scalar(0.15f, 0.75f, second_highest);
    const float single_channel_white = smoothstep_scalar(0.40f, 0.90f, highest)
        * smoothstep_scalar(0.84f, 0.98f, camera_second_highest);
    const float clipped_ratio = max(multi_channel_clip, single_channel_white);
    const float peak = max(scene_linear.x, max(scene_linear.y, scene_linear.z));
    const float highlight_ratio = smoothstep_scalar(0.85f, 1.05f, peak);
    const float blend = clipped_ratio * highlight_ratio;
    return mix(scene_linear, float3(max(0.0f, peak)), blend);
}

// Keep this stencil in the sensor domain: every neighbour is two samples away in both axes, so
// red, green and blue measurements can never be averaged together before demosaic. Its arithmetic
// mirrors raw_denoise.cpp; this implementation difference is intentionally only the executor.
struct RawDenoiseParameters {
    uint storage_width;
    uint storage_height;
    uint active_left;
    uint active_top;
    uint active_right;
    uint active_bottom;
    uint mode;
    uint uses_calibrated_sensor_noise;
    float iso_sensitivity;
    float black_levels[4];
    float white_levels[4];
    float read_noise_stddev_dn[4];
    float shot_noise_variance_per_dn[4];
};

inline float raw_noise_stddev(
    const ushort sample,
    const uint site,
    constant RawDenoiseParameters& parameters
) {
    if (parameters.uses_calibrated_sensor_noise != 0u) {
        const float signal = max(0.0f, float(sample) - parameters.black_levels[site]);
        return sqrt(
            parameters.read_noise_stddev_dn[site] * parameters.read_noise_stddev_dn[site]
            + parameters.shot_noise_variance_per_dn[site] * signal
        );
    }
    const float normalized_iso = parameters.iso_sensitivity > 0.0f
        ? parameters.iso_sensitivity : 100.0f;
    const float iso_multiplier = sqrt(normalized_iso / 100.0f);
    const float range = parameters.white_levels[site] - parameters.black_levels[site];
    return max(1.0f, range * (0.0015f + 0.00035f * iso_multiplier));
}

kernel void denoise_bayer_same_cfa(
    device const ushort* source [[buffer(0)]],
    device ushort* destination [[buffer(1)]],
    constant RawDenoiseParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.storage_width || position.y >= parameters.storage_height
        || position.x < parameters.active_left || position.x >= parameters.active_right
        || position.y < parameters.active_top || position.y >= parameters.active_bottom) {
        return;
    }

    const uint site = cfa_site(position.x, position.y);
    const uint source_index = position.y * parameters.storage_width + position.x;
    const ushort center = source[source_index];
    const float sigma = raw_noise_stddev(center, site, parameters);
    const int radius = parameters.mode == 2u ? 2 : 1;
    const float range_scale = parameters.mode == 2u ? 3.0f : 2.25f;
    const float blend = parameters.mode == 2u ? 0.90f : 0.62f;
    const float range = max(1.0f, sigma * range_scale);
    const float inverse_range_squared = 1.0f / (range * range);
    float weighted_total = 0.0f;
    float total_weight = 0.0f;

    for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
        const int candidate_y = int(position.y) + offset_y * 2;
        if (candidate_y < int(parameters.active_top)
            || candidate_y >= int(parameters.active_bottom)) {
            continue;
        }
        for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
            if (parameters.mode == 1u && abs(offset_x) + abs(offset_y) > 1) {
                continue;
            }
            const int candidate_x = int(position.x) + offset_x * 2;
            if (candidate_x < int(parameters.active_left)
                || candidate_x >= int(parameters.active_right)) {
                continue;
            }
            const ushort candidate = source[
                uint(candidate_y) * parameters.storage_width + uint(candidate_x)
            ];
            const float delta = float(candidate) - float(center);
            const float spatial = 1.0f / float(
                1 + offset_x * offset_x + offset_y * offset_y
            );
            const float range_weight = 1.0f / (1.0f + delta * delta * inverse_range_squared);
            const float weight = spatial * range_weight;
            weighted_total += float(candidate) * weight;
            total_weight += weight;
        }
    }
    if (total_weight <= 0.0f || !isfinite(total_weight)) {
        destination[source_index] = center;
        return;
    }
    const float filtered = weighted_total / total_weight;
    const float blended = float(center) + (filtered - float(center)) * blend;
    destination[source_index] = ushort(clamp(floor(blended + 0.5f), 0.0f, 65535.0f));
}

// High-quality detail reconstruction mirrors bayer_sampling.cpp: estimate green along the
// smoothest sensor direction, then interpolate red/blue as local colour differences. The
// bilinear sample remains the edge fallback and the owner of clipping evidence.
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
    const CameraRgbSample camera = parameters.reconstruction_quality == 2u
        ? edge_aware_camera_rgb_at(samples, parameters, raw_x, raw_y)
        : camera_rgb_at(samples, parameters, raw_x, raw_y);
    if (parameters.project_sensor_clipping != 0u) {
        clipping_output[position.y * parameters.output_width + output_x] =
            sensor_clipping_flags(clipping_source, parameters, output_x, output_y);
    }
    const float red =
        parameters.camera_to_linear_srgb[0] * camera.values.x
        + parameters.camera_to_linear_srgb[1] * camera.values.y
        + parameters.camera_to_linear_srgb[2] * camera.values.z;
    const float green =
        parameters.camera_to_linear_srgb[3] * camera.values.x
        + parameters.camera_to_linear_srgb[4] * camera.values.y
        + parameters.camera_to_linear_srgb[5] * camera.values.z;
    const float blue =
        parameters.camera_to_linear_srgb[6] * camera.values.x
        + parameters.camera_to_linear_srgb[7] * camera.values.y
        + parameters.camera_to_linear_srgb[8] * camera.values.z;
    float3 scene_linear = float3(red, green, blue);
    if (parameters.neutralize_sensor_highlights != 0u) {
        scene_linear = neutralize_sensor_clipped_highlight(scene_linear, camera);
    }
    const uint output_index =
        (position.y * parameters.output_width + output_x) * 3u;
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
    float clipped_weights[3] = {0.0f, 0.0f, 0.0f};
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
            clipped_weights[channel] += sensor_clip_evidence(normalized) * weight;
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
    const CameraRgbSample camera = (
        weights[0] <= 0.0f || weights[1] <= 0.0f || weights[2] <= 0.0f
    ) ? camera_rgb_at(samples, parameters, center_x, center_y) : CameraRgbSample{
        float3(
            totals[0] / weights[0],
            totals[1] / weights[1],
            totals[2] / weights[2]
        ),
        float3(
            clipped_weights[0] / weights[0],
            clipped_weights[1] / weights[1],
            clipped_weights[2] / weights[2]
        )
    };
    if (parameters.project_sensor_clipping != 0u) {
        clipping_output[position.y * parameters.output_width + output_x] =
            sensor_clipping_flags(clipping_source, parameters, output_x, output_y);
    }
    const float red =
        parameters.camera_to_linear_srgb[0] * camera.values.x
        + parameters.camera_to_linear_srgb[1] * camera.values.y
        + parameters.camera_to_linear_srgb[2] * camera.values.z;
    const float green =
        parameters.camera_to_linear_srgb[3] * camera.values.x
        + parameters.camera_to_linear_srgb[4] * camera.values.y
        + parameters.camera_to_linear_srgb[5] * camera.values.z;
    const float blue =
        parameters.camera_to_linear_srgb[6] * camera.values.x
        + parameters.camera_to_linear_srgb[7] * camera.values.y
        + parameters.camera_to_linear_srgb[8] * camera.values.z;
    float3 scene_linear = float3(red, green, blue);
    if (parameters.neutralize_sensor_highlights != 0u) {
        scene_linear = neutralize_sensor_clipped_highlight(scene_linear, camera);
    }
    const uint output_index = (position.y * parameters.output_width + output_x) * 3u;
    output[output_index] = scene_linear.x;
    output[output_index + 1u] = scene_linear.y;
    output[output_index + 2u] = scene_linear.z;
}

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

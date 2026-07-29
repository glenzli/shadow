#pragma once

namespace shadow::image::detail {

// Shared Metal ABI, Bayer sampling, sensor-clipping projection, and highlight recovery. More
// specialised kernel fragments build on this contract and are composed by
// metal_raw_development_msl.hpp into one library.
inline constexpr char metal_raw_common_source[] = R"METAL(
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

)METAL";

} // namespace shadow::image::detail

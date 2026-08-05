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
    float camera_neutral[3];
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
    return clamp((normalized - 0.999f) * 1000.0f, 0.0f, 1.0f);
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

inline float3 recover_sensor_clipped_camera_neutral(
    const CameraRgbSample camera,
    constant RawDevelopmentParameters& parameters
) {
    const float3 neutral = float3(
        parameters.camera_neutral[0],
        parameters.camera_neutral[1],
        parameters.camera_neutral[2]
    );
    const float3 balanced = camera.values / neutral;
    const float common_clip = min(1.0f / neutral.x, min(1.0f / neutral.y, 1.0f / neutral.z));
    if (max(balanced.x, max(balanced.y, balanced.z)) <= common_clip) {
        return camera.values;
    }
    const float3 clipped = min(balanced, float3(common_clip));
    const float3 original_opponent = float3(
        balanced.x + balanced.y + balanced.z,
        1.7320508075688772f * (balanced.x - balanced.y),
        -balanced.x - balanced.y + 2.0f * balanced.z
    );
    const float3 clipped_opponent = float3(
        clipped.x + clipped.y + clipped.z,
        1.7320508075688772f * (clipped.x - clipped.y),
        -clipped.x - clipped.y + 2.0f * clipped.z
    );
    const float original_chroma = dot(original_opponent.yz, original_opponent.yz);
    if (original_chroma <= 1.0e-12f) {
        return camera.values;
    }
    const float chroma_ratio = clamp(
        sqrt(dot(clipped_opponent.yz, clipped_opponent.yz) / original_chroma),
        0.0f,
        1.0f
    );
    const float2 recovered_chroma = original_opponent.yz * chroma_ratio;
    const float3 recovered_balanced = float3(
        (original_opponent.x + 0.8660254037844386f * recovered_chroma.x
         - 0.5f * recovered_chroma.y) / 3.0f,
        (original_opponent.x - 0.8660254037844386f * recovered_chroma.x
         - 0.5f * recovered_chroma.y) / 3.0f,
        (original_opponent.x + recovered_chroma.y) / 3.0f
    );
    return max(recovered_balanced * neutral, float3(0.0f));
}

)METAL";

} // namespace shadow::image::detail

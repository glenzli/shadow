#pragma once

namespace shadow::image::detail {

// Shared Metal ABI, Bayer sampling, and sensor-clipping projection. More specialised kernel
// fragments build on this contract and are composed by
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
    uint project_sensor_clipping;
    uint reconstruction_quality;
    uint cfa_channels[4];
    float black_levels[4];
    float white_minus_black[4];
    float camera_to_linear_srgb[9];
    float cfa_white_balance[4];
    uint apply_cfa_white_balance;
    uint reconstruct_cfa_highlights;
};

inline uint cfa_site(uint x, uint y) {
    return ((y & 1u) * 2u) + (x & 1u);
}

inline float normalized_sensor_sample(
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

inline float normalized_sample(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint x,
    uint y
) {
    const uint site = cfa_site(x, y);
    float normalized = normalized_sensor_sample(samples, parameters, x, y);
    if (parameters.apply_cfa_white_balance != 0u) {
        normalized *= parameters.cfa_white_balance[site];
    }
    return normalized;
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
    float4 cfa_highlight_risk;
};

inline float cfa_highlight_risk(const float normalized_sensor) {
    const float t = clamp((normalized_sensor - 0.88f) / 0.12f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float4 cfa_highlight_risk_footprint_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint raw_x,
    uint raw_y
) {
    const uint max_even_x = (parameters.storage_width - 2u) & ~1u;
    const uint max_even_y = (parameters.storage_height - 2u) & ~1u;
    const uint base_x = min(raw_x & ~1u, max_even_x);
    const uint base_y = min(raw_y & ~1u, max_even_y);
    return float4(
        cfa_highlight_risk(normalized_sensor_sample(samples, parameters, base_x, base_y)),
        cfa_highlight_risk(normalized_sensor_sample(samples, parameters, base_x + 1u, base_y)),
        cfa_highlight_risk(normalized_sensor_sample(samples, parameters, base_x, base_y + 1u)),
        cfa_highlight_risk(normalized_sensor_sample(samples, parameters, base_x + 1u, base_y + 1u))
    );
}

inline CameraRgbSample reconstruct_cfa_highlights(
    CameraRgbSample camera,
    constant RawDevelopmentParameters& parameters
) {
    if (parameters.reconstruct_cfa_highlights == 0u) {
        return camera;
    }
    const float exhausted_fraction = dot(clamp(camera.cfa_highlight_risk, 0.0f, 1.0f), float4(0.25f));
    const float multiple_t = clamp((exhausted_fraction - 0.25f) / 0.25f, 0.0f, 1.0f);
    const float white_ceiling_strength = multiple_t * multiple_t * (3.0f - 2.0f * multiple_t);
    if (white_ceiling_strength <= 0.0f) {
        return camera;
    }
    // Match raw_highlight_reconstruction.cpp exactly: first apply H=0-style component clipping,
    // then a deliberately small residual stabilization only for unequal CFA headroom.
    const float peak_before_ceiling = max(camera.values.x, max(camera.values.y, camera.values.z));
    if (peak_before_ceiling <= 1.0f) {
        return camera;
    }
    camera.values += (min(camera.values, float3(1.0f)) - camera.values) * white_ceiling_strength;
    const float a = parameters.camera_to_linear_srgb[0];
    const float b = parameters.camera_to_linear_srgb[1];
    const float c = parameters.camera_to_linear_srgb[2];
    const float d = parameters.camera_to_linear_srgb[3];
    const float e = parameters.camera_to_linear_srgb[4];
    const float f = parameters.camera_to_linear_srgb[5];
    const float g = parameters.camera_to_linear_srgb[6];
    const float h = parameters.camera_to_linear_srgb[7];
    const float i = parameters.camera_to_linear_srgb[8];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g)
                              + c * (d * h - e * g);
    if (!isfinite(determinant) || abs(determinant) < 1.0e-9f) {
        return camera;
    }
    const float3 neutral_direction = float3(
        ((e * i - f * h) + (c * h - b * i) + (b * f - c * e)) / determinant,
        ((f * g - d * i) + (a * i - c * g) + (c * d - a * f)) / determinant,
        ((d * h - e * g) + (b * g - a * h) + (a * e - b * d)) / determinant
    );
    if (!isfinite(neutral_direction.x) || !isfinite(neutral_direction.y)
        || !isfinite(neutral_direction.z) || neutral_direction.x <= 0.0f
        || neutral_direction.y <= 0.0f || neutral_direction.z <= 0.0f) {
        return camera;
    }
    const float3 scene = float3(
        dot(float3(a, b, c), camera.values),
        dot(float3(d, e, f), camera.values),
        dot(float3(g, h, i), camera.values)
    );
    const float energy = max(0.0f, (scene.x + scene.y + scene.z) / 3.0f);
    const float minimum_risk = min(
        min(camera.cfa_highlight_risk.x, camera.cfa_highlight_risk.y),
        min(camera.cfa_highlight_risk.z, camera.cfa_highlight_risk.w)
    );
    const float maximum_risk = max(
        max(camera.cfa_highlight_risk.x, camera.cfa_highlight_risk.y),
        max(camera.cfa_highlight_risk.z, camera.cfa_highlight_risk.w)
    );
    const float imbalance_t = clamp((maximum_risk - minimum_risk - 0.15f) / 0.60f, 0.0f, 1.0f);
    const float residual_evidence = imbalance_t * imbalance_t * (3.0f - 2.0f * imbalance_t);
    camera.values +=
        (neutral_direction * energy - camera.values) * (0.40f * white_ceiling_strength * residual_evidence);
    return camera;
}

inline CameraRgbSample camera_rgb_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint raw_x,
    uint raw_y
) {
    float totals[3] = {0.0f, 0.0f, 0.0f};
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
            counts[channel] += 1u;
        }
    }
    return CameraRgbSample{
        float3(
            totals[0] / float(counts[0]),
            totals[1] / float(counts[1]),
            totals[2] / float(counts[2])
        ),
        cfa_highlight_risk_footprint_at(samples, parameters, raw_x, raw_y)
    };
}

)METAL";

} // namespace shadow::image::detail

#pragma once

#include <string_view>

namespace shadow::image::detail {

inline constexpr std::string_view metal_scene_linear_region_optics_msl = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct MetalSceneLinearRegionOpticsParameters {
    uint full_width;
    uint full_height;
    uint source_origin_x;
    uint source_origin_y;
    uint source_width;
    uint source_height;
    uint output_origin_x;
    uint output_origin_y;
    uint output_width;
    uint output_height;
    uint has_source;
    uint coordinate_remap;
    uint profile_vignetting;
    float manual_vignette_amount;
    float manual_vignette_midpoint;
};

inline bool absolute_coordinate_in_full_image(
    const float x,
    const float y,
    constant MetalSceneLinearRegionOpticsParameters& parameters
) {
    return isfinite(x) && isfinite(y) && x >= 0.0f && y >= 0.0f
        && x <= float(parameters.full_width - 1u)
        && y <= float(parameters.full_height - 1u);
}

inline float profiled_source_value(
    device const float* source,
    device const float* profile_gains,
    constant MetalSceneLinearRegionOpticsParameters& parameters,
    const uint absolute_x,
    const uint absolute_y,
    const uint channel,
    device atomic_uint* failure
) {
    if (parameters.has_source == 0u
        || absolute_x < parameters.source_origin_x
        || absolute_y < parameters.source_origin_y) {
        atomic_fetch_or_explicit(failure, 1u, memory_order_relaxed);
        return 0.0f;
    }
    const uint local_x = absolute_x - parameters.source_origin_x;
    const uint local_y = absolute_y - parameters.source_origin_y;
    if (local_x >= parameters.source_width || local_y >= parameters.source_height) {
        atomic_fetch_or_explicit(failure, 1u, memory_order_relaxed);
        return 0.0f;
    }
    const uint index = (local_y * parameters.source_width + local_x) * 3u + channel;
    float value = source[index];
    if (parameters.profile_vignetting != 0u) {
        value *= profile_gains[index];
    }
    if (!isfinite(value)) {
        atomic_fetch_or_explicit(failure, 1u, memory_order_relaxed);
        return 0.0f;
    }
    // Match Lensfun's generic fp32 colour callback: only a profile colour modification clamps
    // negative DCP scene-linear components. A geometry-only pass must preserve them.
    return parameters.profile_vignetting != 0u ? max(value, 0.0f) : value;
}

inline float bilinear_profiled_source_channel(
    device const float* source,
    device const float* profile_gains,
    constant MetalSceneLinearRegionOpticsParameters& parameters,
    const float absolute_x,
    const float absolute_y,
    const uint channel,
    device atomic_uint* failure
) {
    if (!absolute_coordinate_in_full_image(absolute_x, absolute_y, parameters)) {
        return 0.0f;
    }

    // C-a constructs its preimage from this same full-image footprint. Floor is deliberately
    // evaluated before the source origin is subtracted; localizing the float first creates seams.
    const uint x0 = uint(floor(absolute_x));
    const uint y0 = uint(floor(absolute_y));
    const uint x1 = min(x0 + 1u, parameters.full_width - 1u);
    const uint y1 = min(y0 + 1u, parameters.full_height - 1u);
    const float horizontal = absolute_x - float(x0);
    const float vertical = absolute_y - float(y0);
    const float upper_left =
        profiled_source_value(source, profile_gains, parameters, x0, y0, channel, failure);
    const float upper_right =
        profiled_source_value(source, profile_gains, parameters, x1, y0, channel, failure);
    const float lower_left =
        profiled_source_value(source, profile_gains, parameters, x0, y1, channel, failure);
    const float lower_right =
        profiled_source_value(source, profile_gains, parameters, x1, y1, channel, failure);
    return mix(
        mix(upper_left, upper_right, horizontal),
        mix(lower_left, lower_right, horizontal),
        vertical
    );
}

inline float manual_output_vignette_gain(
    constant MetalSceneLinearRegionOpticsParameters& parameters,
    const uint absolute_x,
    const uint absolute_y
) {
    if (parameters.manual_vignette_amount == 0.0f) {
        return 1.0f;
    }
    const float center_x = (float(parameters.full_width) - 1.0f) * 0.5f;
    const float center_y = (float(parameters.full_height) - 1.0f) * 0.5f;
    const float radius_scale = max(1.0f, length(float2(center_x, center_y)));
    const float2 normalized =
        (float2(float(absolute_x), float(absolute_y)) - float2(center_x, center_y))
        / radius_scale;
    const float radius = min(1.0f, length(normalized));
    const float denominator = max(1.0e-6f, 1.0f - parameters.manual_vignette_midpoint);
    const float progress = clamp(
        (radius - parameters.manual_vignette_midpoint) / denominator,
        0.0f,
        1.0f
    );
    const float feathered = progress * progress * (3.0f - 2.0f * progress);
    return exp2(parameters.manual_vignette_amount * feathered * 1.15f);
}

kernel void apply_scene_linear_region_optics(
    device const float* source [[buffer(0)]],
    device const float* absolute_coordinates [[buffer(1)]],
    device const float* profile_gains [[buffer(2)]],
    device float* output [[buffer(3)]],
    device atomic_uint* failure [[buffer(4)]],
    constant MetalSceneLinearRegionOpticsParameters& parameters [[buffer(5)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width || position.y >= parameters.output_height) {
        return;
    }
    const uint pixel = position.y * parameters.output_width + position.x;
    const uint absolute_output_x = parameters.output_origin_x + position.x;
    const uint absolute_output_y = parameters.output_origin_y + position.y;
    const float manual_gain =
        manual_output_vignette_gain(parameters, absolute_output_x, absolute_output_y);

    for (uint channel = 0u; channel < 3u; ++channel) {
        float value = 0.0f;
        if (parameters.coordinate_remap != 0u) {
            const uint coordinate = (pixel * 3u + channel) * 2u;
            value = bilinear_profiled_source_channel(
                source,
                profile_gains,
                parameters,
                absolute_coordinates[coordinate],
                absolute_coordinates[coordinate + 1u],
                channel,
                failure
            );
        } else {
            value = profiled_source_value(
                source,
                profile_gains,
                parameters,
                absolute_output_x,
                absolute_output_y,
                channel,
                failure
            );
        }
        const float corrected = value * manual_gain;
        if (!isfinite(corrected)) {
            atomic_fetch_or_explicit(failure, 1u, memory_order_relaxed);
            output[pixel * 3u + channel] = 0.0f;
        } else {
            output[pixel * 3u + channel] = corrected;
        }
    }
}
)METAL";

} // namespace shadow::image::detail

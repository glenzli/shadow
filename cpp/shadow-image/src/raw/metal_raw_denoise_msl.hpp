#pragma once

namespace shadow::image::detail {

// Same-CFA sensor-domain denoise. Its arithmetic mirrors raw_denoise.cpp; only the executor
// differs, so reconstruction can consume the resident sensor plane without a host round trip.
inline constexpr char metal_raw_denoise_source[] = R"METAL(
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

)METAL";

} // namespace shadow::image::detail

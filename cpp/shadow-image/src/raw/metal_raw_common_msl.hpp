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
    float linear_response_minus_black[4];
    float camera_to_linear_srgb[9];
    float cfa_white_balance[4];
    uint apply_cfa_white_balance;
    float cfa_white_balance_scale;
    uint cap_physical_sensor_white;
    uint feather_highlight_chroma_neutralization;
    uint has_linear_response_limits;
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

inline float normalized_linear_response_sample(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint x,
    uint y
) {
    if (parameters.has_linear_response_limits == 0u) {
        return normalized_sensor_sample(samples, parameters, x, y);
    }
    const uint site = cfa_site(x, y);
    const uint sample_index = y * parameters.storage_width + x;
    return (float(samples[sample_index]) - parameters.black_levels[site])
        / parameters.linear_response_minus_black[site];
}

inline float normalized_sample(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint x,
    uint y
) {
    const uint site = cfa_site(x, y);
    const float sensor_normalized = normalized_sensor_sample(samples, parameters, x, y);
    float normalized = sensor_normalized;
    if (parameters.apply_cfa_white_balance != 0u) {
        normalized *= parameters.cfa_white_balance[site] * parameters.cfa_white_balance_scale;
    }
    if (parameters.cap_physical_sensor_white != 0u && sensor_normalized >= 1.0f) {
        normalized = min(normalized, 1.0f);
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
    float3 highlight_channel_evidence;
    float3 physical_white_coverage;
    float highlight_chroma_neutralization;
};

inline bool physical_sensor_white(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint x,
    const uint y
) {
    return parameters.cap_physical_sensor_white != 0u
        && normalized_sensor_sample(samples, parameters, x, y) >= 1.0f;
}

// Each CFA colour carries continuous sensor-headroom evidence. The default starts near calibrated
// white so two diverging saturated channels cannot retain a false magenta/cyan ratio until the
// final code value. The opt-in repair widens this shoulder but still invents neither hue nor detail.
inline float highlight_sensor_evidence(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint x,
    const uint y
) {
    if (parameters.cap_physical_sensor_white == 0u) {
        return 0.0f;
    }
    const float normalized = normalized_linear_response_sample(samples, parameters, x, y);
    const float shoulder_start = parameters.feather_highlight_chroma_neutralization != 0u
        ? 0.88f : 0.92f;
    const float t = clamp(
        (normalized - shoulder_start) / (1.0f - shoulder_start),
        0.0f,
        1.0f
    );
    return t * t * (3.0f - 2.0f * t);
}

inline float feathered_highlight_sensor_evidence(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    const uint center_x,
    const uint center_y,
    const uint radius
) {
    float total_weight = 0.0f;
    float evidence_weight = 0.0f;
    for (int dy = -int(radius); dy <= int(radius); ++dy) {
        for (int dx = -int(radius); dx <= int(radius); ++dx) {
            const int candidate_x = int(center_x) + dx;
            const int candidate_y = int(center_y) + dy;
            if (candidate_x < 0 || candidate_y < 0
                || candidate_x >= int(parameters.storage_width)
                || candidate_y >= int(parameters.storage_height)) {
                continue;
            }
            const float x_weight = float(radius + 1u - uint(abs(dx)));
            const float y_weight = float(radius + 1u - uint(abs(dy)));
            const float weight = x_weight * y_weight;
            total_weight += weight;
            evidence_weight += weight * highlight_sensor_evidence(
                samples,
                parameters,
                uint(candidate_x),
                uint(candidate_y)
            );
        }
    }
    return total_weight <= 0.0f ? 0.0f : evidence_weight / total_weight;
}

inline float3 neutralize_untrusted_camera_highlight_chroma(
    const float3 camera_rgb,
    const float chroma_neutralization
) {
    const float weight = clamp(chroma_neutralization, 0.0f, 1.0f);
    if (weight == 0.0f) {
        return camera_rgb;
    }
    const float luminance = dot(camera_rgb, float3(0.25f, 0.5f, 0.25f));
    return mix(camera_rgb, float3(luminance), weight);
}

inline float shared_physical_white_neutralization(const float3 physical_white_coverage) {
    return clamp(
        min(physical_white_coverage.x, min(
            physical_white_coverage.y,
            physical_white_coverage.z
        )),
        0.0f,
        1.0f
    );
}

inline float highlight_chroma_neutralization(
    const float3 values,
    const float3 channel_evidence
) {
    const float first = min(channel_evidence.x, min(channel_evidence.y, channel_evidence.z));
    const float third = max(channel_evidence.x, max(channel_evidence.y, channel_evidence.z));
    const float second = channel_evidence.x + channel_evidence.y + channel_evidence.z - first - third;
    const float evidence_imbalance = third - first;
    const float peak_signal = max(values.x, max(values.y, values.z));
    const float t = clamp((peak_signal - 0.55f) / 0.30f, 0.0f, 1.0f);
    return second * evidence_imbalance * t * t * (3.0f - 2.0f * t);
}

inline float aggressive_highlight_edge_support(const float3 values) {
    const float peak_signal = max(values.x, max(values.y, values.z));
    const float t = clamp((peak_signal - 0.45f) / 0.35f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float highlight_chroma_risk(const float coverage) {
    const float bounded = clamp(coverage, 0.0f, 1.0f);
    const float remaining = 1.0f - bounded;
    return 1.0f - remaining * remaining * remaining * remaining;
}

inline float aggressive_highlight_chroma_risk(
    const float near_white_evidence,
    const float exact_physical_white_coverage
) {
    return max(
        highlight_chroma_risk(exact_physical_white_coverage),
        0.85f * clamp(near_white_evidence, 0.0f, 1.0f)
    );
}

inline CameraRgbSample camera_rgb_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint raw_x,
    uint raw_y
) {
    float totals[3] = {0.0f, 0.0f, 0.0f};
    uint counts[3] = {0u, 0u, 0u};
    float channel_evidence_totals[3] = {0.0f, 0.0f, 0.0f};
    uint channel_at_white[3] = {0u, 0u, 0u};
    uint observed = 0u;
    uint at_white = 0u;
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
            const float channel_evidence =
                highlight_sensor_evidence(samples, parameters, x, y);
            channel_evidence_totals[channel] += channel_evidence;
            counts[channel] += 1u;
            observed += 1u;
            const bool is_at_white = physical_sensor_white(samples, parameters, x, y);
            channel_at_white[channel] += is_at_white ? 1u : 0u;
            at_white += is_at_white ? 1u : 0u;
        }
    }
    const float3 values = float3(
        totals[0] / float(counts[0]),
        totals[1] / float(counts[1]),
        totals[2] / float(counts[2])
    );
    const float3 channel_evidence = float3(
        channel_evidence_totals[0] / float(counts[0]),
        channel_evidence_totals[1] / float(counts[1]),
        channel_evidence_totals[2] / float(counts[2])
    );
    const float3 channel_physical_white_coverage = float3(
        float(channel_at_white[0]) / float(counts[0]),
        float(channel_at_white[1]) / float(counts[1]),
        float(channel_at_white[2]) / float(counts[2])
    );
    const float exact_physical_white_coverage = observed == 0u
        ? 0.0f : float(at_white) / float(observed);
    const float physical_white_coverage = parameters.feather_highlight_chroma_neutralization != 0u
        ? feathered_highlight_sensor_evidence(samples, parameters, raw_x, raw_y, 3u)
        : exact_physical_white_coverage;
    return CameraRgbSample{
        values,
        channel_evidence,
        channel_physical_white_coverage,
        parameters.feather_highlight_chroma_neutralization != 0u
            ? max(
                  max(
                      highlight_chroma_neutralization(values, channel_evidence),
                      shared_physical_white_neutralization(channel_physical_white_coverage)
                  ),
                  aggressive_highlight_chroma_risk(
                      physical_white_coverage,
                      exact_physical_white_coverage
                  ) * aggressive_highlight_edge_support(values)
              )
            : max(
                  highlight_chroma_neutralization(values, channel_evidence),
                  shared_physical_white_neutralization(channel_physical_white_coverage)
              )
    };
}

)METAL";

} // namespace shadow::image::detail

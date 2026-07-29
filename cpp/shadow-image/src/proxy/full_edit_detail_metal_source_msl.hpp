#pragma once

#include <string_view>

namespace shadow::image::detail {

inline constexpr std::string_view full_edit_detail_metal_source_msl = R"metal(
#include <metal_stdlib>
using namespace metal;

struct SourceCurveSegment {
    float x0;
    float x1;
    float y0;
    float y1;
    float m0;
    float m1;
};

struct SourceRenderingParameters {
    uint width;
    uint height;
    uint row_floats;
    uint segment_count;
    float exposure_gain;
    float hdr_handoff_width;
    float hdr_terminal_output;
    float hdr_terminal_slope;
};

float monotone_curve_value(
    device const SourceCurveSegment* segments,
    const uint segment_count,
    const float input
) {
    if (segment_count == 0u) {
        return input;
    }
    if (input <= segments[0].x0) {
        return segments[0].y0;
    }
    const SourceCurveSegment last = segments[segment_count - 1u];
    if (input >= last.x1) {
        return last.y1;
    }
    for (uint index = 0u; index < segment_count; ++index) {
        const SourceCurveSegment segment = segments[index];
        if (input > segment.x1) {
            continue;
        }
        const float interval = segment.x1 - segment.x0;
        const float t = (input - segment.x0) / interval;
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        const float result = h00 * segment.y0 + h10 * interval * segment.m0
            + h01 * segment.y1 + h11 * interval * segment.m1;
        return clamp(result, segment.y0, segment.y1);
    }
    return last.y1;
}

float source_curve_value(
    device const SourceCurveSegment* segments,
    const SourceRenderingParameters parameters,
    const float input
) {
    if (input <= 1.0f) {
        return monotone_curve_value(segments, parameters.segment_count, input);
    }
    const float handoff_end = 1.0f + parameters.hdr_handoff_width;
    if (input >= handoff_end) {
        return input;
    }
    const float y0 = parameters.hdr_terminal_output;
    const float y1 = handoff_end;
    const float span = y1 - y0;
    const float t = (input - 1.0f) / parameters.hdr_handoff_width;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    const float normalized_start_slope = min(
        parameters.hdr_terminal_slope * parameters.hdr_handoff_width / span,
        2.0f
    );
    const float normalized_end_slope = min(parameters.hdr_handoff_width / span, 1.0f);
    const float result = h00 * y0 + h10 * span * normalized_start_slope
        + h01 * y1 + h11 * span * normalized_end_slope;
    return clamp(result, y0, y1);
}

kernel void apply_full_edit_source_rendering(
    device float* source [[buffer(0)]],
    device const SourceCurveSegment* segments [[buffer(1)]],
    constant SourceRenderingParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint index = position.y * parameters.row_floats + position.x * 3u;
    float3 value = float3(source[index], source[index + 1u], source[index + 2u])
        * parameters.exposure_gain;
    if (parameters.segment_count != 0u) {
        const float3 positive = max(value, float3(0.0f));
        const float luminance = dot(positive, float3(0.2126f, 0.7152f, 0.0722f));
        if (luminance > 1.0e-12f) {
            value *= source_curve_value(segments, parameters, luminance) / luminance;
        }
    }
    source[index] = value.x;
    source[index + 1u] = value.y;
    source[index + 2u] = value.z;
}
)metal";

} // namespace shadow::image::detail

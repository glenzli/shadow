#include "bayer_sampling.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] int rgb_channel(const RawCfaColor color) noexcept {
    switch (color) {
    case RawCfaColor::red:
        return 0;
    case RawCfaColor::green:
        return 1;
    case RawCfaColor::blue:
        return 2;
    case RawCfaColor::unknown:
        return -1;
    }
    return -1;
}

[[nodiscard]] std::size_t cfa_site(const std::uint32_t raw_x, const std::uint32_t raw_y) noexcept {
    return static_cast<std::size_t>((raw_y & 1U) * 2U + (raw_x & 1U));
}

[[nodiscard]] RawCfaColor cfa_color_at(
    const RawFrameDescriptor& descriptor,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    return descriptor.bayer_2x2[cfa_site(raw_x, raw_y)];
}

[[nodiscard]] float normalized_sensor_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto site = cfa_site(raw_x, raw_y);
    const auto width = static_cast<std::size_t>(descriptor.storage_dimensions.width);
    const auto index = static_cast<std::size_t>(raw_y) * width + raw_x;
    const double black = descriptor.black_levels[site];
    const double white = descriptor.white_levels[site];
    const double normalized = (static_cast<double>(frame.samples[index]) - black) / (white - black);
    return static_cast<float>(normalized);
}

[[nodiscard]] float normalized_linear_response_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto site = cfa_site(raw_x, raw_y);
    if (!descriptor.has_linear_response_limits) {
        return normalized_sensor_sample(frame, raw_x, raw_y);
    }
    const auto width = static_cast<std::size_t>(descriptor.storage_dimensions.width);
    const auto index = static_cast<std::size_t>(raw_y) * width + raw_x;
    const double black = descriptor.black_levels[site];
    const double limit = descriptor.linear_response_limits[site];
    return static_cast<float>(
        (static_cast<double>(frame.samples[index]) - black) / (limit - black)
    );
}

[[nodiscard]] bool physical_sensor_white(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    return sampling_policy.cap_physical_sensor_white
           && normalized_sensor_sample(frame, raw_x, raw_y) >= 1.0F;
}

[[nodiscard]] bool shared_terminal_cfa_footprint(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    const auto& descriptor = frame.descriptor;
    std::array<std::uint32_t, 3U> observed{};
    std::array<std::uint32_t, 3U> terminal{};
    for (int dy = -1; dy <= 1; ++dy) {
        const int candidate_y = static_cast<int>(raw_y) + dy;
        if (candidate_y < 0
            || candidate_y >= static_cast<int>(descriptor.storage_dimensions.height)) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            const int candidate_x = static_cast<int>(raw_x) + dx;
            if (candidate_x < 0
                || candidate_x >= static_cast<int>(descriptor.storage_dimensions.width)) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(candidate_x);
            const auto y = static_cast<std::uint32_t>(candidate_y);
            const int channel = rgb_channel(cfa_color_at(descriptor, x, y));
            if (channel < 0) {
                continue;
            }
            ++observed[static_cast<std::size_t>(channel)];
            terminal[static_cast<std::size_t>(channel)] +=
                normalized_sensor_sample(frame, x, y) >= 1.0F ? 1U : 0U;
        }
    }
    return std::ranges::all_of(observed, [](const std::uint32_t count) { return count > 0U; })
           && std::equal(observed.begin(), observed.end(), terminal.begin());
}

[[nodiscard]] float unreconstructed_normalized_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    const auto site = cfa_site(raw_x, raw_y);
    const float sensor_normalized = normalized_sensor_sample(frame, raw_x, raw_y);
    double normalized = sensor_normalized;
    if (transform != nullptr && transform->apply_cfa_white_balance) {
        normalized *= transform->cfa_white_balance[site] * sampling_policy.white_balance_scale;
    }
    // Do not turn a white-balance gain into an early source clip. A sample below physical sensor
    // white may legitimately become greater than one and must reach the scene-linear edit graph.
    // A sensor-white sample has no additional measured response, but its white-balanced fp32 value
    // still carries scene-referred energy. Editable RAW retains that domain for every CFA phase and
    // carries physical-white topology separately into the bounded shared-core source shoulder.
    // Diagnostics may request a shared-only projection, but production does not reintroduce a
    // Bayer-aligned 1.0 step at isolated terminal sites.
    if (sampling_policy.cap_physical_sensor_white && sensor_normalized >= 1.0F) {
        const bool retain_headroom = sampling_policy.preserve_terminal_white_balance_headroom
                                     && (!sampling_policy.require_shared_terminal_headroom
                                         || shared_terminal_cfa_footprint(frame, raw_x, raw_y));
        if (!retain_headroom) {
            normalized = std::min(normalized, 1.0);
        }
    }
    return static_cast<float>(normalized);
}

[[nodiscard]] float normalized_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    return opposed_highlight_cfa_sample_at(frame, raw_x, raw_y, transform, sampling_policy)
        .reconstructed;
}

[[nodiscard]] bool in_sensor_bounds(
    const RawFrameDescriptor& descriptor,
    const std::int64_t raw_x,
    const std::int64_t raw_y
) noexcept {
    return raw_x >= 0 && raw_y >= 0
           && raw_x < static_cast<std::int64_t>(descriptor.storage_dimensions.width)
           && raw_y < static_cast<std::int64_t>(descriptor.storage_dimensions.height);
}

struct PhysicalWhiteWindowCoverage final {
    CameraRgb channels{};
    CameraRgb highlight_channel_evidence{};
    float total = 0.0F;
};

[[nodiscard]] float highlight_sensor_evidence(
    const RawFrame& frame,
    std::uint32_t raw_x,
    std::uint32_t raw_y,
    BayerCfaSamplingPolicy sampling_policy
) noexcept;

[[nodiscard]] PhysicalWhiteWindowCoverage physical_sensor_white_coverage_in_window(
    const RawFrame& frame,
    const std::uint32_t center_x,
    const std::uint32_t center_y,
    const std::int32_t radius,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    const auto& descriptor = frame.descriptor;
    std::array<std::uint32_t, 3U> channel_observed{};
    std::array<std::uint32_t, 3U> channel_at_white{};
    std::array<double, 3U> channel_evidence_totals{};
    std::uint32_t observed = 0U;
    std::uint32_t at_white = 0U;
    for (std::int32_t dy = -radius; dy <= radius; ++dy) {
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            const auto candidate_x = static_cast<std::int64_t>(center_x) + dx;
            const auto candidate_y = static_cast<std::int64_t>(center_y) + dy;
            if (!in_sensor_bounds(descriptor, candidate_x, candidate_y)) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(candidate_x);
            const auto y = static_cast<std::uint32_t>(candidate_y);
            const int channel = rgb_channel(cfa_color_at(descriptor, x, y));
            if (channel < 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(channel);
            ++observed;
            ++channel_observed[index];
            const float channel_evidence = highlight_sensor_evidence(frame, x, y, sampling_policy);
            channel_evidence_totals[index] += channel_evidence;
            const bool is_at_white = physical_sensor_white(frame, x, y, sampling_policy);
            at_white += is_at_white ? 1U : 0U;
            channel_at_white[index] += is_at_white ? 1U : 0U;
        }
    }
    PhysicalWhiteWindowCoverage coverage;
    coverage.total =
        observed == 0U ? 0.0F : static_cast<float>(at_white) / static_cast<float>(observed);
    for (std::size_t channel = 0U; channel < coverage.channels.size(); ++channel) {
        coverage.channels[channel] = channel_observed[channel] == 0U
                                         ? 0.0F
                                         : static_cast<float>(channel_at_white[channel])
                                               / static_cast<float>(channel_observed[channel]);
        coverage.highlight_channel_evidence[channel] =
            channel_observed[channel] == 0U ? 0.0F
                                            : static_cast<float>(
                                                  channel_evidence_totals[channel]
                                                  / static_cast<double>(channel_observed[channel])
                                              );
    }
    return coverage;
}

// The default path deliberately keeps the exact contributing CFA footprint. The opt-in repair
// path instead feathers only a bounded sensor-headroom confidence over the neighbourhood. It
// gives physical white full confidence, then deliberately treats the last 10% of measured sensor
// headroom as uncertain colour evidence. This makes the neutral pull continuous at a blown
// boundary without borrowing a neighbour's hue or fabricating luminance/detail.
[[nodiscard]] float highlight_sensor_evidence(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    if (!sampling_policy.cap_physical_sensor_white) {
        return 0.0F;
    }
    const float normalized = normalized_linear_response_sample(frame, raw_x, raw_y);
    // The default starts continuously reducing chroma confidence before a channel reaches its
    // calibrated white.  A strict physical-white gate left a red/blue pair completely intact
    // until the final code value, which is exactly the magenta contour a deep highlight pull can
    // reveal.  The optional destructive repair broadens only this evidence shoulder.
    const float shoulder_start =
        sampling_policy.feather_highlight_chroma_neutralization ? 0.88F : 0.92F;
    const float t = std::clamp((normalized - shoulder_start) / (1.0F - shoulder_start), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float feathered_highlight_sensor_evidence(
    const RawFrame& frame,
    const std::uint32_t center_x,
    const std::uint32_t center_y,
    const std::uint32_t radius,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    const auto& descriptor = frame.descriptor;
    double total_weight = 0.0;
    double evidence_weight = 0.0;
    for (std::int32_t dy = -static_cast<std::int32_t>(radius);
         dy <= static_cast<std::int32_t>(radius);
         ++dy) {
        for (std::int32_t dx = -static_cast<std::int32_t>(radius);
             dx <= static_cast<std::int32_t>(radius);
             ++dx) {
            const auto candidate_x = static_cast<std::int64_t>(center_x) + dx;
            const auto candidate_y = static_cast<std::int64_t>(center_y) + dy;
            if (!in_sensor_bounds(descriptor, candidate_x, candidate_y)) {
                continue;
            }
            const auto x_weight =
                static_cast<double>(static_cast<std::int32_t>(radius) + 1 - std::abs(dx));
            const auto y_weight =
                static_cast<double>(static_cast<std::int32_t>(radius) + 1 - std::abs(dy));
            const double weight = x_weight * y_weight;
            total_weight += weight;
            evidence_weight += weight
                               * static_cast<double>(highlight_sensor_evidence(
                                   frame,
                                   static_cast<std::uint32_t>(candidate_x),
                                   static_cast<std::uint32_t>(candidate_y),
                                   sampling_policy
                               ));
        }
    }
    return total_weight <= 0.0 ? 0.0F : static_cast<float>(evidence_weight / total_weight);
}

[[nodiscard]] float highlight_chroma_neutralization(
    const CameraRgb& values,
    const CameraRgb& channel_evidence
) noexcept {
    // A single clipped colour can be a real coloured emitter.  Once a second independently
    // sampled CFA colour loses headroom, however, their relative chroma is no longer measured.
    // The middle evidence value is therefore a continuous two-channel gate, not the old discrete
    // "several clipped sites" threshold.  It remains zero for a single saturated red/green/blue
    // source, while a red+blue or green+blue false-colour shoulder converges toward neutral.
    CameraRgb ordered = channel_evidence;
    std::sort(ordered.begin(), ordered.end());
    const float second_channel_evidence = ordered[1U];
    // Equal headroom loss in all three channels is a neutral sensor shoulder. It has no
    // untrustworthy *ratio* to repair and, before the physical ceiling, may simply reflect the
    // RAW white-balance scale. Only diverging channel headroom exposes the false-colour evidence.
    const float evidence_imbalance = ordered[2U] - ordered[0U];
    const float peak_signal = *std::max_element(values.begin(), values.end());
    const float t = std::clamp((peak_signal - 0.55F) / 0.30F, 0.0F, 1.0F);
    const float bright_support = t * t * (3.0F - 2.0F * t);
    return second_channel_evidence * evidence_imbalance * bright_support;
}

[[nodiscard]] float
shared_physical_white_neutralization(const CameraRgb& physical_white_coverage) noexcept {
    // Once every CFA colour contributes physical-white samples, their reconstructed ratio is no
    // longer measured. This shared term closes the equal-evidence hole left by the older
    // disagreement-only gate, while remaining exactly zero for one- and two-colour emitters.
    return std::clamp(
        *std::min_element(physical_white_coverage.begin(), physical_white_coverage.end()),
        0.0F,
        1.0F
    );
}

[[nodiscard]] float aggressive_highlight_edge_support(const CameraRgb& values) noexcept {
    // A clipped highlight beside a dark edge can have modest average luminance even while one
    // reconstructed channel is at the sensor ceiling. Peak support keeps the opt-in feather
    // active on that boundary without spreading it into genuinely dark neighbouring pixels.
    const float peak_signal = *std::max_element(values.begin(), values.end());
    const float t = std::clamp((peak_signal - 0.45F) / 0.35F, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float highlight_chroma_risk(const float coverage) noexcept {
    // One clipped CFA contribution can invalidate the reconstructed colour difference of its
    // whole Bayer cell. Convert fractional footprint coverage to the chance that at least one of
    // four CFA sites contributing to a camera-RGB estimate is clipped. This remains continuous at
    // footprint boundaries, unlike a pixel-level "any channel" gate.
    const float bounded = std::clamp(coverage, 0.0F, 1.0F);
    const float remaining = 1.0F - bounded;
    return 1.0F - remaining * remaining * remaining * remaining;
}

[[nodiscard]] float saturation_frontier_bilinear_blend(
    const CameraRgb& values,
    const CameraRgb& physical_white_coverage
) noexcept {
    // Directional colour-difference interpolation assumes that every sample in its stencil is a
    // measured response. Once even one CFA colour reaches physical white, the gradient comparison
    // can lock onto Bayer phase instead of the scene edge and emit the horizontal "teeth" that a
    // deep highlight pull exposes. The bilinear estimate is already available in the edge-aware
    // path, so fall back to it only on the bright side of that saturated frontier. This adds no
    // sensor read, pass, buffer, or transfer and leaves measured dark-edge texture untouched.
    const float clipped_channel_support =
        *std::max_element(physical_white_coverage.begin(), physical_white_coverage.end());
    const float topology_t = std::clamp(clipped_channel_support / 0.08F, 0.0F, 1.0F);
    const float topology_support = topology_t * topology_t * (3.0F - 2.0F * topology_t);
    const float peak = *std::max_element(values.begin(), values.end());
    const float bright_t = std::clamp((peak - 0.35F) / 0.40F, 0.0F, 1.0F);
    const float bright_support = bright_t * bright_t * (3.0F - 2.0F * bright_t);
    return topology_support * bright_support;
}

[[nodiscard]] float aggressive_highlight_chroma_risk(
    const float near_white_evidence,
    const float exact_physical_white_coverage
) noexcept {
    // Never undo the default's physical-white containment where a clipped CFA site contributed.
    // The optional extension only adds a capped, continuous near-white rim around that baseline.
    return std::max(
        highlight_chroma_risk(exact_physical_white_coverage),
        0.85F * std::clamp(near_white_evidence, 0.0F, 1.0F)
    );
}

[[nodiscard]] std::optional<float> directional_green_estimate(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto center_colour = cfa_color_at(descriptor, raw_x, raw_y);
    if (center_colour == RawCfaColor::green) {
        return normalized_sample(frame, raw_x, raw_y, transform, sampling_policy);
    }
    if (center_colour != RawCfaColor::red && center_colour != RawCfaColor::blue) {
        return std::nullopt;
    }

    const auto try_direction =
        [&](const std::int64_t dx,
            const std::int64_t dy) -> std::optional<std::pair<float, float>> {
        const auto left_x = static_cast<std::int64_t>(raw_x) - dx;
        const auto left_y = static_cast<std::int64_t>(raw_y) - dy;
        const auto right_x = static_cast<std::int64_t>(raw_x) + dx;
        const auto right_y = static_cast<std::int64_t>(raw_y) + dy;
        const auto far_left_x = static_cast<std::int64_t>(raw_x) - 2 * dx;
        const auto far_left_y = static_cast<std::int64_t>(raw_y) - 2 * dy;
        const auto far_right_x = static_cast<std::int64_t>(raw_x) + 2 * dx;
        const auto far_right_y = static_cast<std::int64_t>(raw_y) + 2 * dy;
        if (!in_sensor_bounds(descriptor, left_x, left_y)
            || !in_sensor_bounds(descriptor, right_x, right_y)
            || !in_sensor_bounds(descriptor, far_left_x, far_left_y)
            || !in_sensor_bounds(descriptor, far_right_x, far_right_y)) {
            return std::nullopt;
        }
        const auto as_u32 = [](const std::int64_t coordinate) noexcept {
            return static_cast<std::uint32_t>(coordinate);
        };
        if (cfa_color_at(descriptor, as_u32(left_x), as_u32(left_y)) != RawCfaColor::green
            || cfa_color_at(descriptor, as_u32(right_x), as_u32(right_y)) != RawCfaColor::green
            || cfa_color_at(descriptor, as_u32(far_left_x), as_u32(far_left_y)) != center_colour
            || cfa_color_at(descriptor, as_u32(far_right_x), as_u32(far_right_y))
                   != center_colour) {
            return std::nullopt;
        }
        const float left =
            normalized_sample(frame, as_u32(left_x), as_u32(left_y), transform, sampling_policy);
        const float right =
            normalized_sample(frame, as_u32(right_x), as_u32(right_y), transform, sampling_policy);
        const float far_left = normalized_sample(
            frame,
            as_u32(far_left_x),
            as_u32(far_left_y),
            transform,
            sampling_policy
        );
        const float far_right = normalized_sample(
            frame,
            as_u32(far_right_x),
            as_u32(far_right_y),
            transform,
            sampling_policy
        );
        const float center = normalized_sample(frame, raw_x, raw_y, transform, sampling_policy);
        const float chroma_laplacian = 2.0F * center - far_left - far_right;
        const float estimate = 0.5F * (left + right) + 0.25F * chroma_laplacian;
        const float gradient = std::abs(left - right) + std::abs(chroma_laplacian);
        return std::pair<float, float>{estimate, gradient};
    };

    const auto horizontal = try_direction(1, 0);
    const auto vertical = try_direction(0, 1);
    if (horizontal.has_value() && vertical.has_value()) {
        constexpr float epsilon = 1.0e-5F;
        const float horizontal_weight = 1.0F / (epsilon + horizontal->second);
        const float vertical_weight = 1.0F / (epsilon + vertical->second);
        return (horizontal->first * horizontal_weight + vertical->first * vertical_weight)
               / (horizontal_weight + vertical_weight);
    }
    if (horizontal.has_value()) {
        return horizontal->first;
    }
    if (vertical.has_value()) {
        return vertical->first;
    }
    return std::nullopt;
}

} // namespace

namespace {

[[nodiscard]] std::optional<float> opposed_reference_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    const auto& descriptor = frame.descriptor;
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const auto candidate_x = static_cast<std::int64_t>(raw_x) + dx;
            const auto candidate_y = static_cast<std::int64_t>(raw_y) + dy;
            if (!in_sensor_bounds(descriptor, candidate_x, candidate_y)) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(candidate_x);
            const auto y = static_cast<std::uint32_t>(candidate_y);
            const int channel = rgb_channel(cfa_color_at(descriptor, x, y));
            if (channel < 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(channel);
            totals[index] += std::max(
                0.0,
                static_cast<double>(
                    unreconstructed_normalized_sample(frame, x, y, transform, sampling_policy)
                )
            );
            ++counts[index];
        }
    }

    const int current_channel = rgb_channel(cfa_color_at(descriptor, raw_x, raw_y));
    if (current_channel < 0) {
        return std::nullopt;
    }
    const auto channel = static_cast<std::size_t>(current_channel);
    const auto first_opposing = (channel + 1U) % 3U;
    const auto second_opposing = (channel + 2U) % 3U;
    if (counts[first_opposing] == 0U || counts[second_opposing] == 0U) {
        return std::nullopt;
    }
    const double first_mean = totals[first_opposing] / counts[first_opposing];
    const double second_mean = totals[second_opposing] / counts[second_opposing];
    const double opposing_root_mean = 0.5 * (std::cbrt(first_mean) + std::cbrt(second_mean));
    return static_cast<float>(opposing_root_mean * opposing_root_mean * opposing_root_mean);
}

} // namespace

CfaOpposedHighlightSample opposed_highlight_cfa_sample_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) noexcept {
    const float measured =
        unreconstructed_normalized_sample(frame, raw_x, raw_y, transform, sampling_policy);
    CfaOpposedHighlightSample result{
        .measured = measured,
        .opposed_reference = measured,
        .reconstructed = measured,
    };
    if (!sampling_policy.cap_physical_sensor_white
        || !sampling_policy.reconstruct_terminal_highlights
        || normalized_sensor_sample(frame, raw_x, raw_y) < 0.987F) {
        return result;
    }

    // Match darktable's Bayer opposed owner at the same semantic boundary: reconstruct only the
    // terminal CFA photosite, before any bilinear, directional, or area aggregation. A 3x3
    // superpixel supplies one mean per CFA colour; the current colour is estimated from the cube of
    // the mean of the two opposing cube roots. `max` keeps the operation one-sided, so a saturated
    // coloured emitter and already-brighter response are never pulled down. The scene-global
    // chrominance offset used by darktable is intentionally diagnostic-only here: it can amplify a
    // terminal CFA discontinuity into a visible colour ring instead of preserving local colour.
    const auto reference = opposed_reference_at(frame, raw_x, raw_y, transform, sampling_policy);
    if (!reference.has_value()) {
        return result;
    }
    result.opposed_reference = *reference;
    result.reconstructed = std::max(measured, *reference);
    result.terminal_candidate = true;
    return result;
}

CfaOpposedChrominanceCorrection estimate_opposed_highlight_chrominance_correction(
    const RawFrame& frame,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy,
    const std::uint32_t support_cell_stride
) {
    validate_bayer_frame(frame, "opposed CFA highlight chrominance estimation");
    if (support_cell_stride == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "opposed CFA highlight chrominance estimation requires a non-zero support stride"
        );
    }
    constexpr float terminal_threshold = 0.987F;
    constexpr float measured_support_begin = 0.2F * terminal_threshold;
    constexpr std::uint32_t block_extent = 3U;
    constexpr std::uint32_t dilation_radius = 3U;
    const auto& descriptor = frame.descriptor;
    const std::uint32_t first_x = descriptor.active_margins.left;
    const std::uint32_t first_y = descriptor.active_margins.top;
    const std::uint32_t active_width = descriptor.active_dimensions.width;
    const std::uint32_t active_height = descriptor.active_dimensions.height;
    const std::uint32_t block_width = (active_width + block_extent - 1U) / block_extent;
    const std::uint32_t block_height = (active_height + block_extent - 1U) / block_extent;
    const std::size_t block_count = static_cast<std::size_t>(block_width) * block_height;
    std::vector<std::uint8_t> terminal_mask(block_count * 3U);
    CfaOpposedChrominanceCorrection correction;

    for (std::uint32_t active_y = 0U; active_y < active_height; ++active_y) {
        const std::uint32_t raw_y = first_y + active_y;
        for (std::uint32_t active_x = 0U; active_x < active_width; ++active_x) {
            const std::uint32_t raw_x = first_x + active_x;
            if (normalized_sensor_sample(frame, raw_x, raw_y) < terminal_threshold) {
                continue;
            }
            const int channel = rgb_channel(cfa_color_at(descriptor, raw_x, raw_y));
            if (channel < 0) {
                continue;
            }
            correction.any_terminal_photosite = true;
            const auto block = static_cast<std::size_t>(active_y / block_extent) * block_width
                               + active_x / block_extent;
            terminal_mask[static_cast<std::size_t>(channel) * block_count + block] = 1U;
        }
    }
    if (!correction.any_terminal_photosite) {
        return correction;
    }

    // A separable radius-three maximum is a bounded square approximation of darktable's dilated
    // clipping neighbourhood and selects measured colour immediately around clipping. Prefix sums
    // make this explicit diagnostic pass linear instead of searching a 7x7 block window for every
    // CFA photosite.
    std::vector<std::uint8_t> dilated_mask(block_count * 3U);
    std::vector<std::uint8_t> horizontal(block_count);
    std::vector<std::uint32_t> prefix(
        static_cast<std::size_t>(std::max(block_width, block_height)) + 1U
    );
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto* source = terminal_mask.data() + channel * block_count;
        auto* destination = dilated_mask.data() + channel * block_count;
        for (std::uint32_t block_y = 0U; block_y < block_height; ++block_y) {
            prefix[0U] = 0U;
            const auto row = static_cast<std::size_t>(block_y) * block_width;
            for (std::uint32_t block_x = 0U; block_x < block_width; ++block_x) {
                prefix[block_x + 1U] = prefix[block_x] + source[row + block_x];
            }
            for (std::uint32_t block_x = 0U; block_x < block_width; ++block_x) {
                const auto begin = block_x > dilation_radius ? block_x - dilation_radius : 0U;
                const auto end = std::min(block_width, block_x + dilation_radius + 1U);
                horizontal[row + block_x] = prefix[end] != prefix[begin] ? 1U : 0U;
            }
        }
        for (std::uint32_t block_x = 0U; block_x < block_width; ++block_x) {
            prefix[0U] = 0U;
            for (std::uint32_t block_y = 0U; block_y < block_height; ++block_y) {
                prefix[block_y + 1U] =
                    prefix[block_y]
                    + horizontal[static_cast<std::size_t>(block_y) * block_width + block_x];
            }
            for (std::uint32_t block_y = 0U; block_y < block_height; ++block_y) {
                const auto begin = block_y > dilation_radius ? block_y - dilation_radius : 0U;
                const auto end = std::min(block_height, block_y + dilation_radius + 1U);
                destination[static_cast<std::size_t>(block_y) * block_width + block_x] =
                    prefix[end] != prefix[begin] ? 1U : 0U;
            }
        }
    }

    std::array<long double, 3U> sums{};
    auto reference_policy = sampling_policy;
    const std::uint32_t cell_width = (active_width + 1U) / 2U;
    const std::uint32_t cell_height = (active_height + 1U) / 2U;
    for (std::uint32_t cell_y = 0U; cell_y < cell_height; cell_y += support_cell_stride) {
        for (std::uint32_t cell_x = 0U; cell_x < cell_width; cell_x += support_cell_stride) {
            for (std::uint32_t phase_y = 0U; phase_y < 2U; ++phase_y) {
                const std::uint32_t active_y = cell_y * 2U + phase_y;
                if (active_y >= active_height) {
                    continue;
                }
                const std::uint32_t raw_y = first_y + active_y;
                for (std::uint32_t phase_x = 0U; phase_x < 2U; ++phase_x) {
                    const std::uint32_t active_x = cell_x * 2U + phase_x;
                    if (active_x >= active_width) {
                        continue;
                    }
                    const std::uint32_t raw_x = first_x + active_x;
                    const float sensor = normalized_sensor_sample(frame, raw_x, raw_y);
                    if (sensor <= measured_support_begin || sensor >= terminal_threshold) {
                        continue;
                    }
                    const int current_channel = rgb_channel(cfa_color_at(descriptor, raw_x, raw_y));
                    if (current_channel < 0) {
                        continue;
                    }
                    const auto channel = static_cast<std::size_t>(current_channel);
                    const auto block =
                        static_cast<std::size_t>(active_y / block_extent) * block_width
                        + active_x / block_extent;
                    if (dilated_mask[channel * block_count + block] == 0U) {
                        continue;
                    }
                    const auto reference =
                        opposed_reference_at(frame, raw_x, raw_y, transform, reference_policy);
                    if (!reference.has_value()) {
                        continue;
                    }
                    const float measured = unreconstructed_normalized_sample(
                        frame,
                        raw_x,
                        raw_y,
                        transform,
                        reference_policy
                    );
                    sums[channel] += static_cast<long double>(measured - *reference);
                    ++correction.supporting_samples[channel];
                }
            }
        }
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        if (correction.supporting_samples[channel] > 100U) {
            correction.offsets[channel] = static_cast<float>(
                sums[channel] / static_cast<long double>(correction.supporting_samples[channel])
            );
        }
    }
    return correction;
}

BayerCfaSamplingPolicy
editable_raw_cfa_sampling_policy(const RawFrameLinearTransform& transform) noexcept {
    BayerCfaSamplingPolicy policy;
    // Physical sensor headroom comes from calibrated CFA black/white levels, not from whether a
    // particular RAW white-balance transform is active. Keep the source evidence available for
    // camera-neutral and manual-WB inputs as well; only the gain normalisation itself is optional.
    policy.cap_physical_sensor_white = true;
    policy.preserve_terminal_white_balance_headroom = true;
    policy.require_shared_terminal_headroom = false;
    policy.reconstruct_terminal_highlights = true;
    if (transform.apply_cfa_white_balance) {
        const auto minimum = *std::min_element(
            transform.cfa_white_balance.begin(),
            transform.cfa_white_balance.end()
        );
        policy.white_balance_scale = static_cast<float>(1.0 / minimum);
    }
    return policy;
}

BayerCfaSamplingPolicy
aggressive_highlight_repair_cfa_sampling_policy(const RawFrameLinearTransform& transform) noexcept {
    auto policy = editable_raw_cfa_sampling_policy(transform);
    policy.feather_highlight_chroma_neutralization = policy.cap_physical_sensor_white;
    return policy;
}

void validate_bayer_frame(const RawFrame& frame, const char* operation) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            std::string(operation) + " requires a valid owned RAW frame"
        );
    }
    if (!frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            std::string(operation) + " requires an explicit Bayer two-by-two CFA layout"
        );
    }
    if (frame.descriptor.storage_dimensions.width < 2U
        || frame.descriptor.storage_dimensions.height < 2U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            std::string(operation) + " requires at least a two-by-two stored Bayer sensor plane"
        );
    }
}

CameraRgbSample bilinear_camera_rgb_sample_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) {
    const auto& descriptor = frame.descriptor;
    const auto width = descriptor.storage_dimensions.width;
    const auto height = descriptor.storage_dimensions.height;
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    std::array<double, 3U> channel_evidence_totals{};
    std::array<std::uint32_t, 3U> channel_at_white{};
    CameraRgbSample result;
    std::uint32_t observed = 0U;
    std::uint32_t at_white = 0U;
    for (int dy = -1; dy <= 1; ++dy) {
        const auto candidate_y = static_cast<std::int64_t>(raw_y) + dy;
        if (candidate_y < 0 || candidate_y >= static_cast<std::int64_t>(height)) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            const auto candidate_x = static_cast<std::int64_t>(raw_x) + dx;
            if (candidate_x < 0 || candidate_x >= static_cast<std::int64_t>(width)) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(candidate_x);
            const auto y = static_cast<std::uint32_t>(candidate_y);
            const int channel = rgb_channel(cfa_color_at(descriptor, x, y));
            if (channel < 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(channel);
            const auto normalized = normalized_sample(frame, x, y, transform, sampling_policy);
            totals[index] += normalized;
            const float channel_evidence = highlight_sensor_evidence(frame, x, y, sampling_policy);
            channel_evidence_totals[index] += channel_evidence;
            ++counts[index];
            ++observed;
            const bool is_at_white = physical_sensor_white(frame, x, y, sampling_policy);
            channel_at_white[index] += is_at_white ? 1U : 0U;
            at_white += is_at_white ? 1U : 0U;
        }
    }

    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        if (counts[channel] == 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "Bayer reconstruction found no same-colour neighbour"
            );
        }
        result.values[channel] =
            static_cast<float>(totals[channel] / static_cast<double>(counts[channel]));
        result.highlight_channel_evidence[channel] = static_cast<float>(
            channel_evidence_totals[channel] / static_cast<double>(counts[channel])
        );
        result.physical_white_coverage[channel] =
            static_cast<float>(channel_at_white[channel]) / static_cast<float>(counts[channel]);
    }
    const float exact_physical_white_coverage =
        observed == 0U ? 0.0F : static_cast<float>(at_white) / static_cast<float>(observed);
    const float physical_white_coverage =
        sampling_policy.feather_highlight_chroma_neutralization
            ? feathered_highlight_sensor_evidence(frame, raw_x, raw_y, 3U, sampling_policy)
            : exact_physical_white_coverage;
    result.highlight_chroma_neutralization = std::max(
        highlight_chroma_neutralization(result.values, result.highlight_channel_evidence),
        shared_physical_white_neutralization(result.physical_white_coverage)
    );
    if (sampling_policy.feather_highlight_chroma_neutralization) {
        result.highlight_chroma_neutralization = std::max(
            result.highlight_chroma_neutralization,
            aggressive_highlight_chroma_risk(physical_white_coverage, exact_physical_white_coverage)
                * aggressive_highlight_edge_support(result.values)
        );
    }
    return result;
}

CameraRgb bilinear_camera_rgb_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) {
    return bilinear_camera_rgb_sample_at(frame, raw_x, raw_y, transform, sampling_policy).values;
}

CameraRgbSample edge_aware_camera_rgb_sample_at(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) {
    const CameraRgbSample bilinear =
        bilinear_camera_rgb_sample_at(frame, raw_x, raw_y, transform, sampling_policy);
    const auto& descriptor = frame.descriptor;
    const auto center_colour = cfa_color_at(descriptor, raw_x, raw_y);
    const int center_channel = rgb_channel(center_colour);
    const auto green = directional_green_estimate(frame, raw_x, raw_y, transform, sampling_policy);
    if (center_channel < 0 || !green.has_value()) {
        return bilinear;
    }

    CameraRgbSample result = bilinear;
    result.values[1U] = *green;
    const auto reconstruct_colour_difference = [&](const RawCfaColor target_colour,
                                                   const std::size_t target_channel) {
        if (center_colour == target_colour) {
            result.values[target_channel] =
                normalized_sample(frame, raw_x, raw_y, transform, sampling_policy);
            return;
        }
        double weighted_sum = 0.0;
        double total_weight = 0.0;
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            const auto candidate_y = static_cast<std::int64_t>(raw_y) + dy;
            if (candidate_y < 0
                || candidate_y >= static_cast<std::int64_t>(descriptor.storage_dimensions.height)) {
                continue;
            }
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const auto candidate_x = static_cast<std::int64_t>(raw_x) + dx;
                if (candidate_x < 0
                    || candidate_x
                           >= static_cast<std::int64_t>(descriptor.storage_dimensions.width)) {
                    continue;
                }
                const auto x = static_cast<std::uint32_t>(candidate_x);
                const auto y = static_cast<std::uint32_t>(candidate_y);
                if (cfa_color_at(descriptor, x, y) != target_colour) {
                    continue;
                }
                const auto neighbour_green =
                    directional_green_estimate(frame, x, y, transform, sampling_policy);
                if (!neighbour_green.has_value()) {
                    continue;
                }
                const double weight = dx == 0 || dy == 0 ? 1.0 : 0.7071067811865476;
                weighted_sum +=
                    weight
                    * (static_cast<double>(
                           normalized_sample(frame, x, y, transform, sampling_policy)
                       )
                       + static_cast<double>(*green) - static_cast<double>(*neighbour_green));
                total_weight += weight;
            }
        }
        if (total_weight > 0.0) {
            result.values[target_channel] = static_cast<float>(weighted_sum / total_weight);
        }
    };
    reconstruct_colour_difference(RawCfaColor::red, 0U);
    reconstruct_colour_difference(RawCfaColor::blue, 2U);

    // The extended topology scan belongs only to the highlight frontier. Ordinary pixels retain
    // the already-computed bilinear evidence, avoiding another 7x7 sensor walk in the hottest
    // reconstruction path. The aggressive threshold matches its zero-support boundary; the
    // default opens only for a plausible final-response-shoulder candidate.
    const float reconstructed_peak = *std::max_element(result.values.begin(), result.values.end());
    const float bilinear_evidence_peak = *std::max_element(
        result.highlight_channel_evidence.begin(),
        result.highlight_channel_evidence.end()
    );
    const float bilinear_white_peak = *std::max_element(
        result.physical_white_coverage.begin(),
        result.physical_white_coverage.end()
    );
    const float extended_scan_threshold =
        sampling_policy.feather_highlight_chroma_neutralization ? 0.45F : 0.90F;
    if (bilinear_evidence_peak <= 0.0F && bilinear_white_peak <= 0.0F
        && reconstructed_peak < extended_scan_threshold) {
        return result;
    }

    // Directional reconstruction reads as far as three sensor sites away. Carry that actual
    // window's per-colour clipping topology instead of inheriting only the initial bilinear 3x3
    // footprint when this pixel can visibly participate in a highlight repair.
    const auto directional_white =
        physical_sensor_white_coverage_in_window(frame, raw_x, raw_y, 3, sampling_policy);
    result.physical_white_coverage = directional_white.channels;
    result.highlight_channel_evidence = directional_white.highlight_channel_evidence;
    const float exact_physical_white_coverage = directional_white.total;
    const float physical_white_coverage =
        sampling_policy.feather_highlight_chroma_neutralization
            ? feathered_highlight_sensor_evidence(frame, raw_x, raw_y, 3U, sampling_policy)
            : exact_physical_white_coverage;
    const float bilinear_blend =
        saturation_frontier_bilinear_blend(bilinear.values, directional_white.channels);
    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        result.values[channel] =
            result.values[channel]
            + bilinear_blend * (bilinear.values[channel] - result.values[channel]);
    }
    result.highlight_chroma_neutralization = std::max(
        highlight_chroma_neutralization(result.values, result.highlight_channel_evidence),
        shared_physical_white_neutralization(result.physical_white_coverage)
    );
    if (sampling_policy.feather_highlight_chroma_neutralization) {
        result.highlight_chroma_neutralization = std::max(
            result.highlight_chroma_neutralization,
            aggressive_highlight_chroma_risk(physical_white_coverage, exact_physical_white_coverage)
                * aggressive_highlight_edge_support(result.values)
        );
    }
    return result;
}

BayerAreaSamplingGrid
make_bayer_area_sampling_grid(const RawFrame& frame, const Dimensions target_dimensions) {
    if (target_dimensions.width == 0U || target_dimensions.height == 0U
        || target_dimensions.width > frame.descriptor.active_dimensions.width
        || target_dimensions.height > frame.descriptor.active_dimensions.height) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer area sampling target is outside the active sensor dimensions"
        );
    }
    return BayerAreaSamplingGrid{
        .target_dimensions = target_dimensions,
        .scale_x = static_cast<double>(frame.descriptor.active_dimensions.width)
                   / static_cast<double>(target_dimensions.width),
        .scale_y = static_cast<double>(frame.descriptor.active_dimensions.height)
                   / static_cast<double>(target_dimensions.height),
    };
}

CameraRgbSample area_camera_rgb_sample_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    const std::uint32_t target_x,
    const std::uint32_t target_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) {
    const auto& descriptor = frame.descriptor;
    // This is also reached by the isolated decode helper.  A malformed provider frame or a
    // rounded last footprint must therefore become a normal DecodeError, never an unchecked
    // read past the owned sensor plane in a worker thread.
    if (grid.target_dimensions.width == 0U || grid.target_dimensions.height == 0U
        || target_x >= grid.target_dimensions.width || target_y >= grid.target_dimensions.height
        || !std::isfinite(grid.scale_x) || !std::isfinite(grid.scale_y) || grid.scale_x <= 0.0
        || grid.scale_y <= 0.0) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer area sampling received an invalid target coordinate or sampling grid"
        );
    }

    const double active_left = static_cast<double>(descriptor.active_margins.left);
    const double active_top = static_cast<double>(descriptor.active_margins.top);
    const double active_right =
        active_left + static_cast<double>(descriptor.active_dimensions.width);
    const double active_bottom =
        active_top + static_cast<double>(descriptor.active_dimensions.height);
    const auto active_right_exclusive = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(descriptor.active_margins.left)
        + descriptor.active_dimensions.width
    );
    const auto active_bottom_exclusive = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(descriptor.active_margins.top)
        + descriptor.active_dimensions.height
    );
    const double unclamped_source_top = active_top + static_cast<double>(target_y) * grid.scale_y;
    const double unclamped_source_bottom =
        active_top + static_cast<double>(target_y + 1U) * grid.scale_y;
    const double unclamped_source_left = active_left + static_cast<double>(target_x) * grid.scale_x;
    const double unclamped_source_right =
        active_left + static_cast<double>(target_x + 1U) * grid.scale_x;
    const double source_top = std::clamp(unclamped_source_top, active_top, active_bottom);
    const double source_bottom = std::clamp(unclamped_source_bottom, active_top, active_bottom);
    const double source_left = std::clamp(unclamped_source_left, active_left, active_right);
    const double source_right = std::clamp(unclamped_source_right, active_left, active_right);
    if (source_left >= source_right || source_top >= source_bottom) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer area sampling footprint is outside the active sensor rectangle"
        );
    }
    // Clamp after ceil as well.  Exact rational scale factors can round one ulp above the active
    // edge, and the previous implementation then dereferenced one sample beyond the final row or
    // column.  The active rectangle is validated to sit inside storage by RawFrame::valid().
    const auto first_source_y = static_cast<std::uint32_t>(std::floor(source_top));
    const auto last_source_y =
        std::min(active_bottom_exclusive, static_cast<std::uint32_t>(std::ceil(source_bottom)));
    const auto first_source_x = static_cast<std::uint32_t>(std::floor(source_left));
    const auto last_source_x =
        std::min(active_right_exclusive, static_cast<std::uint32_t>(std::ceil(source_right)));

    std::array<double, 3U> totals{};
    std::array<double, 3U> weights{};
    std::array<double, 3U> channel_evidence_totals{};
    std::array<double, 3U> channel_physical_white_weights{};
    std::array<double, 3U> damaged_totals{};
    std::array<double, 3U> damaged_weights{};
    std::array<double, 3U> damaged_evidence_totals{};
    std::array<double, 3U> damaged_physical_white_weights{};
    CameraRgbSample result;
    double observed_weight = 0.0;
    double physical_white_weight = 0.0;
    // Treat the response shoulder as a separate, one-sided contribution layer. Neutralizing the
    // already mixed output RGB rewrites reliable dark content in a bin that straddles a lamp;
    // thresholding that mixed tuple also creates a hard band across a broad clipped sky. Keeping
    // trusted and damaged mass separate lets the ordinary area integral anti-alias the boundary,
    // while only photosites that actually lost headroom receive any chroma correction.
    for (std::uint32_t raw_y = first_source_y; raw_y < last_source_y; ++raw_y) {
        const double overlap_y = std::max(
            0.0,
            std::min(source_bottom, static_cast<double>(raw_y + 1U))
                - std::max(source_top, static_cast<double>(raw_y))
        );
        for (std::uint32_t raw_x = first_source_x; raw_x < last_source_x; ++raw_x) {
            const double overlap_x = std::max(
                0.0,
                std::min(source_right, static_cast<double>(raw_x + 1U))
                    - std::max(source_left, static_cast<double>(raw_x))
            );
            const int channel = rgb_channel(cfa_color_at(descriptor, raw_x, raw_y));
            if (channel < 0) {
                continue;
            }
            const auto index = static_cast<std::size_t>(channel);
            const double weight = overlap_x * overlap_y;
            const float evidence =
                highlight_sensor_evidence(frame, raw_x, raw_y, sampling_policy);
            const float measured =
                unreconstructed_normalized_sample(frame, raw_x, raw_y, transform, sampling_policy);
            float reconstructed = measured;
            if (sampling_policy.reconstruct_terminal_highlights && evidence > 0.0F) {
                const auto reference =
                    opposed_reference_at(frame, raw_x, raw_y, transform, sampling_policy);
                if (reference.has_value()) {
                    reconstructed =
                        std::max(measured, std::lerp(measured, *reference, evidence));
                }
            }
            totals[index] += reconstructed * weight;
            weights[index] += weight;
            channel_evidence_totals[index] += static_cast<double>(evidence) * weight;
            observed_weight += weight;
            const bool at_white = physical_sensor_white(frame, raw_x, raw_y, sampling_policy);
            if (at_white) {
                channel_physical_white_weights[index] += weight;
                physical_white_weight += weight;
            }
            if (evidence > 0.0F) {
                const double damaged_weight = static_cast<double>(evidence) * weight;
                damaged_totals[index] += reconstructed * damaged_weight;
                damaged_weights[index] += damaged_weight;
                damaged_evidence_totals[index] +=
                    static_cast<double>(evidence) * damaged_weight;
                damaged_physical_white_weights[index] += at_white ? damaged_weight : 0.0;
            }
        }
    }

    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        if (weights[channel] <= 0.0) {
            const auto center_x = std::min(
                descriptor.storage_dimensions.width - 1U,
                static_cast<std::uint32_t>((source_left + source_right) * 0.5)
            );
            const auto center_y = std::min(
                descriptor.storage_dimensions.height - 1U,
                static_cast<std::uint32_t>((source_top + source_bottom) * 0.5)
            );
            return bilinear_camera_rgb_sample_at(
                frame,
                center_x,
                center_y,
                transform,
                sampling_policy
            );
        }
        result.values[channel] = static_cast<float>(totals[channel] / weights[channel]);
        result.highlight_channel_evidence[channel] =
            static_cast<float>(channel_evidence_totals[channel] / weights[channel]);
        result.physical_white_coverage[channel] =
            static_cast<float>(channel_physical_white_weights[channel] / weights[channel]);
    }
    CameraRgb damaged_values = result.values;
    CameraRgb damaged_channel_evidence{};
    CameraRgb damaged_physical_white_coverage{};
    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        if (damaged_weights[channel] <= 0.0) {
            continue;
        }
        damaged_values[channel] =
            static_cast<float>(damaged_totals[channel] / damaged_weights[channel]);
        damaged_channel_evidence[channel] =
            static_cast<float>(damaged_evidence_totals[channel] / damaged_weights[channel]);
        damaged_physical_white_coverage[channel] = static_cast<float>(
            damaged_physical_white_weights[channel] / damaged_weights[channel]
        );
    }
    const float damaged_neutralization = std::max(
        highlight_chroma_neutralization(damaged_values, damaged_channel_evidence),
        shared_physical_white_neutralization(damaged_physical_white_coverage)
    );
    const float damaged_luminance = 0.25F * damaged_values[0U] + 0.5F * damaged_values[1U]
                                     + 0.25F * damaged_values[2U];
    for (std::size_t channel = 0U; channel < result.values.size(); ++channel) {
        if (damaged_weights[channel] <= 0.0) {
            continue;
        }
        const double corrected_total = totals[channel]
                                       + static_cast<double>(damaged_neutralization)
                                             * (damaged_luminance - damaged_values[channel])
                                             * damaged_weights[channel];
        result.values[channel] =
            static_cast<float>(std::max(0.0, corrected_total) / weights[channel]);
    }
    const auto center_x = std::min(
        descriptor.storage_dimensions.width - 1U,
        static_cast<std::uint32_t>((source_left + source_right) * 0.5)
    );
    const auto center_y = std::min(
        descriptor.storage_dimensions.height - 1U,
        static_cast<std::uint32_t>((source_top + source_bottom) * 0.5)
    );
    const float exact_physical_white_coverage =
        observed_weight <= 0.0 ? 0.0F : static_cast<float>(physical_white_weight / observed_weight);
    const float physical_white_coverage =
        sampling_policy.feather_highlight_chroma_neutralization
            ? feathered_highlight_sensor_evidence(
                  frame,
                  center_x,
                  center_y,
                  std::clamp(
                      static_cast<std::uint32_t>(
                          std::ceil(1.5 * std::max(grid.scale_x, grid.scale_y))
                      ),
                      2U,
                      16U
                  ),
                  sampling_policy
              )
            : exact_physical_white_coverage;
    // Default area development already resolved chroma inside the damaged contribution layer.
    // Keep the post-integral blend disabled: reclassifying the mixed output tuple would recreate
    // the broad colour band this path exists to avoid. Aggressive mode may still add its explicitly
    // wider spatial confidence term below.
    result.highlight_chroma_neutralization = 0.0F;
    if (sampling_policy.feather_highlight_chroma_neutralization) {
        result.highlight_chroma_neutralization = std::max(
            result.highlight_chroma_neutralization,
            aggressive_highlight_chroma_risk(physical_white_coverage, exact_physical_white_coverage)
                * aggressive_highlight_edge_support(result.values)
        );
    }
    return result;
}

CameraRgb area_camera_rgb_at(
    const RawFrame& frame,
    const BayerAreaSamplingGrid& grid,
    const std::uint32_t target_x,
    const std::uint32_t target_y,
    const RawFrameLinearTransform* const transform,
    const BayerCfaSamplingPolicy sampling_policy
) {
    return area_camera_rgb_sample_at(frame, grid, target_x, target_y, transform, sampling_policy)
        .values;
}

} // namespace shadow::image::detail

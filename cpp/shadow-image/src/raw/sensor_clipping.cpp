#include <shadow/image/sensor_clipping.hpp>

#include <shadow/image/decoder_error.hpp>

#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace shadow::image {

namespace {

[[nodiscard]] bool transpose_orientation(const std::int32_t orientation) noexcept {
    return orientation == 5 || orientation == 6;
}

[[nodiscard]] Dimensions
oriented_dimensions(Dimensions dimensions, const std::int32_t orientation) noexcept {
    if (transpose_orientation(orientation)) {
        std::swap(dimensions.width, dimensions.height);
    }
    return dimensions;
}

// `project_sensor_clipping_mask()` needs one exact reduction per displayed
// pixel: any sensor sample at white marks a highlight, while every sample at
// black marks a shadow.  Iterate target bins, rather than source pixels, so
// each worker owns one disjoint output row and can update its flags without
// locks.  These are the inverse ranges of
// `floor(source * target_extent / source_extent)`.
[[nodiscard]] std::uint32_t target_bin_begin(
    const std::uint32_t target_coordinate,
    const std::uint32_t source_extent,
    const std::uint32_t target_extent
) noexcept {
    const std::uint64_t numerator = static_cast<std::uint64_t>(target_coordinate) * source_extent;
    const std::uint64_t quotient = numerator / target_extent;
    const std::uint64_t remainder = numerator % target_extent;
    return static_cast<std::uint32_t>(quotient + (remainder == 0U ? 0U : 1U));
}

[[nodiscard]] std::uint32_t target_bin_end(
    const std::uint32_t target_coordinate,
    const std::uint32_t source_extent,
    const std::uint32_t target_extent
) noexcept {
    const std::uint64_t numerator =
        (static_cast<std::uint64_t>(target_coordinate) + 1U) * source_extent;
    const std::uint64_t quotient = numerator / target_extent;
    const std::uint64_t remainder = numerator % target_extent;
    return static_cast<std::uint32_t>(quotient + (remainder == 0U ? 0U : 1U));
}

[[nodiscard]] Dimensions coordinate_from_display_orientation(
    const Dimensions active_dimensions,
    const std::int32_t orientation,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    switch (orientation) {
    case 3:
        return {
            active_dimensions.width - 1U - x,
            active_dimensions.height - 1U - y,
        };
    case 5: // LibRaw: 90° counterclockwise.
        return {active_dimensions.width - 1U - y, x};
    case 6: // LibRaw: 90° clockwise.
        return {y, active_dimensions.height - 1U - x};
    case 0:
    case 1:
    default:
        return {x, y};
    }
}

[[nodiscard]] std::size_t cfa_site(const std::uint32_t x, const std::uint32_t y) noexcept {
    return static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
}

[[nodiscard]] int rgb_channel(
    const RawFrameDescriptor& descriptor,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    switch (descriptor.bayer_2x2[cfa_site(raw_x, raw_y)]) {
    case RawCfaColor::red:
        return 0;
    case RawCfaColor::green:
        return 1;
    case RawCfaColor::blue:
        return 2;
    case RawCfaColor::unknown:
    default:
        return -1;
    }
}

[[nodiscard]] std::size_t checked_mask_size(const Dimensions dimensions) {
    const auto count = dimensions.pixel_count();
    if (count == 0U
        || count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "sensor clipping mask dimensions exceed addressable memory"
        );
    }
    return static_cast<std::size_t>(count);
}

[[nodiscard]] float normalized_sensor_sample(
    const RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto site = cfa_site(raw_x, raw_y);
    const float black = static_cast<float>(descriptor.black_levels[site]);
    const float white = static_cast<float>(descriptor.white_levels[site]);
    const float sample = static_cast<float>(
        frame.samples[static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x]
    );
    return std::clamp((sample - black) / std::max(white - black, 1.0F), 0.0F, 1.0F);
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
    const float black = static_cast<float>(descriptor.black_levels[site]);
    const float limit = static_cast<float>(descriptor.linear_response_limits[site]);
    const float sample = static_cast<float>(
        frame.samples[static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x]
    );
    return std::clamp((sample - black) / std::max(limit - black, 1.0F), 0.0F, 1.0F);
}

[[nodiscard]] std::array<float, 4U>
canonical_as_shot_balance(const RawFrameDescriptor& descriptor) noexcept {
    const double reference =
        *std::max_element(descriptor.as_shot_neutral.begin(), descriptor.as_shot_neutral.end());
    std::array<float, 4U> balance{};
    for (std::size_t site = 0U; site < balance.size(); ++site) {
        const double neutral = descriptor.as_shot_neutral[site];
        balance[site] = neutral > 0.0 && std::isfinite(neutral) && reference > 0.0
                            ? static_cast<float>(reference / neutral)
                            : 1.0F;
    }
    return balance;
}

[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float value) noexcept {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] std::uint8_t quantized_shared_highlight_coverage(
    const std::array<std::uint32_t, 3U>& observed,
    const std::array<std::uint32_t, 3U>& highlights
) noexcept {
    float coverage = 1.0F;
    for (std::size_t channel = 0U; channel < observed.size(); ++channel) {
        if (observed[channel] == 0U) {
            return 0U;
        }
        coverage = std::min(
            coverage,
            static_cast<float>(highlights[channel]) / static_cast<float>(observed[channel])
        );
    }
    const auto quantized = static_cast<std::uint32_t>(
        std::lround(coverage * static_cast<float>(sensor_shared_highlight_coverage_levels))
    );
    return static_cast<std::uint8_t>(
        std::min<std::uint32_t>(quantized, sensor_shared_highlight_coverage_levels)
    );
}

} // namespace

bool SensorClippingMask::valid() const noexcept {
    const auto count = dimensions.pixel_count();
    if (schema_version != sensor_clipping_mask_schema_version || count == 0U
        || count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || samples.size() != static_cast<std::size_t>(count)) {
        return false;
    }

    std::uint64_t highlights = 0U;
    std::uint64_t shadows = 0U;
    for (const auto sample : samples) {
        const auto coverage =
            static_cast<std::uint8_t>(sample >> sensor_shared_highlight_coverage_shift);
        if (coverage > 0U && (sample & sensor_highlight_clipped) == 0U) {
            return false;
        }
        highlights += (sample & sensor_highlight_clipped) != 0U ? 1U : 0U;
        shadows += (sample & sensor_shadow_clipped) != 0U ? 1U : 0U;
    }
    return highlights == highlight_pixel_count && shadows == shadow_pixel_count;
}

float SensorClippingMask::shared_highlight_coverage_at(const std::size_t pixel) const noexcept {
    if (pixel >= samples.size()) {
        return 0.0F;
    }
    const auto quantized =
        static_cast<std::uint8_t>(samples[pixel] >> sensor_shared_highlight_coverage_shift);
    if (quantized > 0U) {
        return static_cast<float>(quantized)
               / static_cast<float>(sensor_shared_highlight_coverage_levels);
    }
    return (samples[pixel] & sensor_shared_highlight_clipped) != 0U ? 1.0F : 0.0F;
}

bool HighlightChromaRiskMap::valid() const noexcept {
    const auto count = dimensions.pixel_count();
    return schema_version == highlight_chroma_risk_map_schema_version && count != 0U
           && count <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
           && samples.size() == static_cast<std::size_t>(count);
}

SensorClippingMask
project_sensor_clipping_mask(const RawFrame& frame, const Dimensions target_dimensions) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "sensor clipping diagnostics require a valid owned RAW frame"
        );
    }
    const std::size_t target_count = checked_mask_size(target_dimensions);
    const auto& descriptor = frame.descriptor;
    const Dimensions oriented_active =
        oriented_dimensions(descriptor.active_dimensions, descriptor.orientation);
    if (oriented_active.width == 0U || oriented_active.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "sensor clipping diagnostics require a non-empty active RAW frame"
        );
    }

    SensorClippingMask output;
    output.dimensions = target_dimensions;
    output.samples.resize(target_count);

    const auto storage_width = static_cast<std::size_t>(descriptor.storage_dimensions.width);
    detail::parallel_for_rows(
        target_dimensions.height,
        8U,
        [&frame, &output, &descriptor, target_dimensions, oriented_active, storage_width](
            const std::uint32_t first_target_y,
            const std::uint32_t last_target_y
        ) {
            for (std::uint32_t target_y = first_target_y; target_y < last_target_y; ++target_y) {
                const auto oriented_y_begin =
                    target_bin_begin(target_y, oriented_active.height, target_dimensions.height);
                const auto oriented_y_end =
                    target_bin_end(target_y, oriented_active.height, target_dimensions.height);
                for (std::uint32_t target_x = 0U; target_x < target_dimensions.width; ++target_x) {
                    const auto oriented_x_begin =
                        target_bin_begin(target_x, oriented_active.width, target_dimensions.width);
                    const auto oriented_x_end =
                        target_bin_end(target_x, oriented_active.width, target_dimensions.width);
                    bool observed = false;
                    bool all_shadow = true;
                    bool any_highlight = false;
                    std::array<std::uint32_t, 3U> channel_observed{};
                    std::array<std::uint32_t, 3U> channel_highlights{};
                    for (std::uint32_t oriented_y = oriented_y_begin; oriented_y < oriented_y_end;
                         ++oriented_y) {
                        for (std::uint32_t oriented_x = oriented_x_begin;
                             oriented_x < oriented_x_end;
                             ++oriented_x) {
                            const Dimensions active = coordinate_from_display_orientation(
                                descriptor.active_dimensions,
                                descriptor.orientation,
                                oriented_x,
                                oriented_y
                            );
                            const std::uint32_t raw_x =
                                descriptor.active_margins.left + active.width;
                            const std::uint32_t raw_y =
                                descriptor.active_margins.top + active.height;
                            const auto site = cfa_site(raw_x, raw_y);
                            const auto sample =
                                frame.samples
                                    [static_cast<std::size_t>(raw_y) * storage_width + raw_x];
                            observed = true;
                            all_shadow = all_shadow && sample <= descriptor.black_levels[site];
                            any_highlight =
                                any_highlight || sample >= descriptor.white_levels[site];
                            const int channel = rgb_channel(descriptor, raw_x, raw_y);
                            if (channel >= 0) {
                                ++channel_observed[static_cast<std::size_t>(channel)];
                                channel_highlights[static_cast<std::size_t>(channel)] +=
                                    sample >= descriptor.white_levels[site] ? 1U : 0U;
                            }
                        }
                    }

                    // Native-size output cells contain only one CFA site. For the stricter shared
                    // terminal fact, use the same bounded 3x3 colour footprint that feeds the
                    // bilinear reconstruction; downsampled bins already contain their exact area.
                    const bool incomplete_colour_footprint =
                        std::ranges::any_of(channel_observed, [](const std::uint32_t count) {
                            return count == 0U;
                        });
                    if (incomplete_colour_footprint && target_dimensions == oriented_active) {
                        channel_observed.fill(0U);
                        channel_highlights.fill(0U);
                        const std::uint32_t centre_x = oriented_x_begin;
                        const std::uint32_t centre_y = oriented_y_begin;
                        const std::uint32_t support_x_begin = centre_x > 0U ? centre_x - 1U : 0U;
                        const std::uint32_t support_y_begin = centre_y > 0U ? centre_y - 1U : 0U;
                        const std::uint32_t support_x_end =
                            std::min(oriented_active.width, centre_x + 2U);
                        const std::uint32_t support_y_end =
                            std::min(oriented_active.height, centre_y + 2U);
                        for (std::uint32_t support_y = support_y_begin; support_y < support_y_end;
                             ++support_y) {
                            for (std::uint32_t support_x = support_x_begin;
                                 support_x < support_x_end;
                                 ++support_x) {
                                const Dimensions active = coordinate_from_display_orientation(
                                    descriptor.active_dimensions,
                                    descriptor.orientation,
                                    support_x,
                                    support_y
                                );
                                const std::uint32_t raw_x =
                                    descriptor.active_margins.left + active.width;
                                const std::uint32_t raw_y =
                                    descriptor.active_margins.top + active.height;
                                const auto site = cfa_site(raw_x, raw_y);
                                const int channel = rgb_channel(descriptor, raw_x, raw_y);
                                if (channel < 0) {
                                    continue;
                                }
                                const auto source_sample =
                                    frame.samples
                                        [static_cast<std::size_t>(raw_y) * storage_width + raw_x];
                                ++channel_observed[static_cast<std::size_t>(channel)];
                                channel_highlights[static_cast<std::size_t>(channel)] +=
                                    source_sample >= descriptor.white_levels[site] ? 1U : 0U;
                            }
                        }
                    }

                    const bool shared_highlight =
                        std::ranges::all_of(
                            channel_observed,
                            [](const std::uint32_t count) { return count > 0U; }
                        )
                        && std::equal(
                            channel_observed.begin(),
                            channel_observed.end(),
                            channel_highlights.begin()
                        );

                    std::uint8_t flags = 0U;
                    if (observed && all_shadow) {
                        flags = static_cast<std::uint8_t>(flags | sensor_shadow_clipped);
                    }
                    if (any_highlight) {
                        flags = static_cast<std::uint8_t>(flags | sensor_highlight_clipped);
                    }
                    if (shared_highlight) {
                        flags = static_cast<std::uint8_t>(
                            flags | sensor_highlight_clipped | sensor_shared_highlight_clipped
                        );
                    }
                    const auto coverage =
                        quantized_shared_highlight_coverage(channel_observed, channel_highlights);
                    output.samples
                        [static_cast<std::size_t>(target_y) * target_dimensions.width + target_x] =
                        static_cast<std::uint8_t>(
                            flags | (coverage << sensor_shared_highlight_coverage_shift)
                        );
                }
            }
        }
    );

    for (const auto flags : output.samples) {
        output.highlight_pixel_count += (flags & sensor_highlight_clipped) != 0U ? 1U : 0U;
        output.shadow_pixel_count += (flags & sensor_shadow_clipped) != 0U ? 1U : 0U;
    }
    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "sensor clipping diagnostics produced an invalid output mask"
        );
    }
    return output;
}

HighlightChromaRiskMap
project_highlight_chroma_risk_map(const RawFrame& frame, const Dimensions target_dimensions) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "highlight chroma confidence requires a valid owned RAW frame"
        );
    }
    const std::size_t target_count = checked_mask_size(target_dimensions);
    const auto& descriptor = frame.descriptor;
    const Dimensions oriented_active =
        oriented_dimensions(descriptor.active_dimensions, descriptor.orientation);
    if (oriented_active.width == 0U || oriented_active.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "highlight chroma confidence requires a non-empty active RAW frame"
        );
    }

    HighlightChromaRiskMap output;
    output.dimensions = target_dimensions;
    output.samples.resize(target_count);
    const auto as_shot_balance = canonical_as_shot_balance(descriptor);
    detail::parallel_for_rows(
        target_dimensions.height,
        8U,
        [&frame, &output, &descriptor, target_dimensions, oriented_active, as_shot_balance](
            const std::uint32_t first_target_y,
            const std::uint32_t last_target_y
        ) {
            for (std::uint32_t target_y = first_target_y; target_y < last_target_y; ++target_y) {
                const auto oriented_y_begin =
                    target_bin_begin(target_y, oriented_active.height, target_dimensions.height);
                const auto oriented_y_end =
                    target_bin_end(target_y, oriented_active.height, target_dimensions.height);
                for (std::uint32_t target_x = 0U; target_x < target_dimensions.width; ++target_x) {
                    const auto oriented_x_begin =
                        target_bin_begin(target_x, oriented_active.width, target_dimensions.width);
                    const auto oriented_x_end =
                        target_bin_end(target_x, oriented_active.width, target_dimensions.width);
                    std::array<double, 3U> sums{};
                    std::array<double, 3U> weights{};
                    for (std::uint32_t oriented_y = oriented_y_begin; oriented_y < oriented_y_end;
                         ++oriented_y) {
                        for (std::uint32_t oriented_x = oriented_x_begin;
                             oriented_x < oriented_x_end;
                             ++oriented_x) {
                            const Dimensions active = coordinate_from_display_orientation(
                                descriptor.active_dimensions,
                                descriptor.orientation,
                                oriented_x,
                                oriented_y
                            );
                            const std::uint32_t raw_x =
                                descriptor.active_margins.left + active.width;
                            const std::uint32_t raw_y =
                                descriptor.active_margins.top + active.height;
                            const int channel = rgb_channel(descriptor, raw_x, raw_y);
                            if (channel < 0) {
                                continue;
                            }
                            const float response_sample =
                                normalized_linear_response_sample(frame, raw_x, raw_y);
                            const float balanced_sample =
                                normalized_sensor_sample(frame, raw_x, raw_y)
                                * as_shot_balance[cfa_site(raw_x, raw_y)];
                            // The CFA response starts losing chroma reliability before its
                            // terminal plateau.  Begin this continuous shoulder evidence early
                            // enough to cover the Sony-style halo surrounding a shared clipped
                            // core, while the later multi-channel test keeps ordinary bright
                            // colour and a one-colour emitter out of the risk map.
                            const float response_evidence =
                                smoothstep(0.84F, 1.0F, response_sample);
                            // A neutral lamp can place red/blue well below their raw container
                            // ceilings while canonical white balance brings all three colours to
                            // the same bright surface. At a hard edge, independent CFA footprints
                            // then expose green/red/blue sampling phase as horizontal teeth even
                            // though only one raw channel is near its terminal response. The
                            // camera's immutable as-shot balance distinguishes that case from a
                            // genuinely saturated one-colour emitter without depending on the
                            // user's later white-balance edit.
                            const float balanced_surface_evidence =
                                0.92F * smoothstep(0.78F, 1.02F, balanced_sample);
                            const float evidence =
                                std::max(response_evidence, balanced_surface_evidence);
                            sums[static_cast<std::size_t>(channel)] += evidence;
                            weights[static_cast<std::size_t>(channel)] += 1.0;
                        }
                    }
                    std::array<float, 3U> evidence{};
                    for (std::size_t channel = 0U; channel < evidence.size(); ++channel) {
                        evidence[channel] =
                            weights[channel] > 0.0
                                ? static_cast<float>(sums[channel] / weights[channel])
                                : 0.0F;
                    }
                    auto sorted = evidence;
                    std::sort(sorted.begin(), sorted.end());
                    const float two_channel_loss = sorted[1U];
                    const float imbalance = sorted[2U] - sorted[0U];
                    // Divergent multi-channel headroom is the usual false-chroma case. Keeping
                    // it separate protects a genuinely saturated one-colour emitter, whose
                    // least-affected CFA channel remains well below the shoulder.
                    const float disagreement_risk =
                        two_channel_loss * smoothstep(0.02F, 0.35F, imbalance);
                    // A common final shoulder is also chroma-unreliable even when the channels
                    // agree. Sony's sun core exposes this: all three CFA responses flatten
                    // together, so an agreement-only map leaves a false hue for the later
                    // selective-tone pull to reveal. `sorted[0]` is the least-affected channel,
                    // therefore this only activates once every channel is deeply into the final
                    // response shoulder; ordinary highlights and one-colour emitters stay out.
                    const float shared_terminal_risk = smoothstep(0.75F, 0.98F, sorted[0U]);
                    const float risk = std::max(disagreement_risk, shared_terminal_risk);
                    const auto output_index =
                        static_cast<std::size_t>(target_y) * target_dimensions.width + target_x;
                    const auto encoded_risk = static_cast<std::uint8_t>(
                        std::lround(std::clamp(risk, 0.0F, 1.0F) * 255.0F)
                    );
                    output.samples[output_index] = encoded_risk;
                }
            }
        }
    );
    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "highlight chroma confidence produced an invalid output map"
        );
    }
    return output;
}

} // namespace shadow::image

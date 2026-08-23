#include <shadow/image/sensor_clipping.hpp>

#include <shadow/image/decoder_error.hpp>

#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace shadow::image {

namespace {

[[nodiscard]] bool transpose_orientation(const std::int32_t orientation) noexcept {
    return orientation == 5 || orientation == 6;
}

[[nodiscard]] Dimensions oriented_dimensions(
    Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
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
    const std::uint64_t numerator = static_cast<std::uint64_t>(target_coordinate)
        * source_extent;
    const std::uint64_t quotient = numerator / target_extent;
    const std::uint64_t remainder = numerator % target_extent;
    return static_cast<std::uint32_t>(quotient + (remainder == 0U ? 0U : 1U));
}

[[nodiscard]] std::uint32_t target_bin_end(
    const std::uint32_t target_coordinate,
    const std::uint32_t source_extent,
    const std::uint32_t target_extent
) noexcept {
    const std::uint64_t numerator = (static_cast<std::uint64_t>(target_coordinate) + 1U)
        * source_extent;
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
    if (count == 0U || count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
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
    const float sample = static_cast<float>(frame.samples[
        static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x
    ]);
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
    const float sample = static_cast<float>(frame.samples[
        static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x
    ]);
    return std::clamp((sample - black) / std::max(limit - black, 1.0F), 0.0F, 1.0F);
}

[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float value) noexcept {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

} // namespace

bool SensorClippingMask::valid() const noexcept {
    const auto count = dimensions.pixel_count();
    if (
        schema_version != sensor_clipping_mask_schema_version || count == 0U
        || count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || samples.size() != static_cast<std::size_t>(count)
    ) {
        return false;
    }

    std::uint64_t highlights = 0U;
    std::uint64_t shadows = 0U;
    for (const auto sample : samples) {
        if ((sample & ~(sensor_highlight_clipped | sensor_shadow_clipped)) != 0U) {
            return false;
        }
        highlights += (sample & sensor_highlight_clipped) != 0U ? 1U : 0U;
        shadows += (sample & sensor_shadow_clipped) != 0U ? 1U : 0U;
    }
    return highlights == highlight_pixel_count && shadows == shadow_pixel_count;
}

bool HighlightChromaRiskMap::valid() const noexcept {
    const auto count = dimensions.pixel_count();
    return schema_version == highlight_chroma_risk_map_schema_version && count != 0U
           && count <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
           && samples.size() == static_cast<std::size_t>(count);
}

SensorClippingMask project_sensor_clipping_mask(
    const RawFrame& frame,
    const Dimensions target_dimensions
) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "sensor clipping diagnostics require a valid owned RAW frame"
        );
    }
    const std::size_t target_count = checked_mask_size(target_dimensions);
    const auto& descriptor = frame.descriptor;
    const Dimensions oriented_active = oriented_dimensions(
        descriptor.active_dimensions,
        descriptor.orientation
    );
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
        [&frame,
         &output,
         &descriptor,
         target_dimensions,
         oriented_active,
         storage_width](const std::uint32_t first_target_y, const std::uint32_t last_target_y) {
            for (std::uint32_t target_y = first_target_y;
                 target_y < last_target_y;
                 ++target_y) {
                const auto oriented_y_begin = target_bin_begin(
                    target_y,
                    oriented_active.height,
                    target_dimensions.height
                );
                const auto oriented_y_end = target_bin_end(
                    target_y,
                    oriented_active.height,
                    target_dimensions.height
                );
                for (std::uint32_t target_x = 0U;
                     target_x < target_dimensions.width;
                     ++target_x) {
                    const auto oriented_x_begin = target_bin_begin(
                        target_x,
                        oriented_active.width,
                        target_dimensions.width
                    );
                    const auto oriented_x_end = target_bin_end(
                        target_x,
                        oriented_active.width,
                        target_dimensions.width
                    );
                    bool observed = false;
                    bool all_shadow = true;
                    bool any_highlight = false;
                    for (std::uint32_t oriented_y = oriented_y_begin;
                         oriented_y < oriented_y_end;
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
                            const std::uint32_t raw_x = descriptor.active_margins.left + active.width;
                            const std::uint32_t raw_y = descriptor.active_margins.top + active.height;
                            const auto site = cfa_site(raw_x, raw_y);
                            const auto sample = frame.samples[
                                static_cast<std::size_t>(raw_y) * storage_width + raw_x
                            ];
                            observed = true;
                            all_shadow = all_shadow && sample <= descriptor.black_levels[site];
                            any_highlight = any_highlight
                                || sample >= descriptor.white_levels[site];
                        }
                    }

                    std::uint8_t flags = 0U;
                    if (observed && all_shadow) {
                        flags = static_cast<std::uint8_t>(flags | sensor_shadow_clipped);
                    }
                    if (any_highlight) {
                        flags = static_cast<std::uint8_t>(flags | sensor_highlight_clipped);
                    }
                    output.samples[
                        static_cast<std::size_t>(target_y) * target_dimensions.width + target_x
                    ] = flags;
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

// The risk map drives a selective neutral pull later in the RAW pipeline.  A
// terminally clipped component can end abruptly beside a valid pixel after
// preview downsampling, which makes that pull look like a contour when the
// user lowers highlights.  Feather only that terminal component by one output
// pixel while preparing the source.  The early, CFA-disagreement shoulder is
// deliberately left untouched: spreading it would desaturate ordinary bright
// colour and repeat the midtone side effect this map is meant to avoid.
void feather_terminal_highlight_chroma_boundaries(HighlightChromaRiskMap& map) {
    constexpr std::uint8_t terminal_seed = 224U;
    constexpr float axial_transfer = 0.56F;
    constexpr float diagonal_transfer = 0.36F;
    constexpr std::array<std::array<int, 2U>, 8U> neighbours{{
        {{-1, -1}}, {{0, -1}}, {{1, -1}}, {{-1, 0}},
        {{1, 0}}, {{-1, 1}}, {{0, 1}}, {{1, 1}},
    }};

    const auto source_samples = map.samples;
    const auto width = map.dimensions.width;
    const auto height = map.dimensions.height;
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            auto feathered = source_samples[index];
            for (const auto& offset : neighbours) {
                const auto neighbour_x = static_cast<std::int64_t>(x) + offset[0];
                const auto neighbour_y = static_cast<std::int64_t>(y) + offset[1];
                if (neighbour_x < 0 || neighbour_y < 0
                    || neighbour_x >= static_cast<std::int64_t>(width)
                    || neighbour_y >= static_cast<std::int64_t>(height)) {
                    continue;
                }
                const auto neighbour = source_samples[
                    static_cast<std::size_t>(neighbour_y) * width
                    + static_cast<std::uint32_t>(neighbour_x)
                ];
                if (neighbour < terminal_seed) {
                    continue;
                }
                const auto transfer = offset[0] == 0 || offset[1] == 0
                                          ? axial_transfer : diagonal_transfer;
                feathered = std::max(
                    feathered,
                    static_cast<std::uint8_t>(std::lround(static_cast<float>(neighbour) * transfer))
                );
            }
            map.samples[index] = feathered;
        }
    }
}

HighlightChromaRiskMap project_highlight_chroma_risk_map(
    const RawFrame& frame,
    const Dimensions target_dimensions
) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "highlight chroma confidence requires a valid owned RAW frame"
        );
    }
    const std::size_t target_count = checked_mask_size(target_dimensions);
    const auto& descriptor = frame.descriptor;
    const Dimensions oriented_active = oriented_dimensions(
        descriptor.active_dimensions,
        descriptor.orientation
    );
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
    detail::parallel_for_rows(
        target_dimensions.height,
        8U,
        [&frame, &output, &descriptor, target_dimensions, oriented_active](
            const std::uint32_t first_target_y,
            const std::uint32_t last_target_y
        ) {
            for (std::uint32_t target_y = first_target_y; target_y < last_target_y; ++target_y) {
                const auto oriented_y_begin = target_bin_begin(
                    target_y, oriented_active.height, target_dimensions.height
                );
                const auto oriented_y_end = target_bin_end(
                    target_y, oriented_active.height, target_dimensions.height
                );
                for (std::uint32_t target_x = 0U; target_x < target_dimensions.width; ++target_x) {
                    const auto oriented_x_begin = target_bin_begin(
                        target_x, oriented_active.width, target_dimensions.width
                    );
                    const auto oriented_x_end = target_bin_end(
                        target_x, oriented_active.width, target_dimensions.width
                    );
                    std::array<double, 3U> sums{};
                    std::array<double, 3U> weights{};
                    for (std::uint32_t oriented_y = oriented_y_begin;
                         oriented_y < oriented_y_end;
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
                            const std::uint32_t raw_x = descriptor.active_margins.left + active.width;
                            const std::uint32_t raw_y = descriptor.active_margins.top + active.height;
                            const int channel = rgb_channel(descriptor, raw_x, raw_y);
                            if (channel < 0) {
                                continue;
                            }
                            const float sample = normalized_linear_response_sample(
                                frame,
                                raw_x,
                                raw_y
                            );
                            // The CFA response starts losing chroma reliability before its
                            // terminal plateau.  Begin this continuous shoulder evidence early
                            // enough to cover the Sony-style halo surrounding a shared clipped
                            // core, while the later multi-channel test keeps ordinary bright
                            // colour and a one-colour emitter out of the risk map.
                            const float evidence = smoothstep(0.84F, 1.0F, sample);
                            sums[static_cast<std::size_t>(channel)] += evidence;
                            weights[static_cast<std::size_t>(channel)] += 1.0;
                        }
                    }
                    std::array<float, 3U> evidence{};
                    for (std::size_t channel = 0U; channel < evidence.size(); ++channel) {
                        evidence[channel] = weights[channel] > 0.0
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
                    const float disagreement_risk = two_channel_loss
                                                    * smoothstep(0.02F, 0.35F, imbalance);
                    // A common final shoulder is also chroma-unreliable even when the channels
                    // agree. Sony's sun core exposes this: all three CFA responses flatten
                    // together, so an agreement-only map leaves a false hue for the later
                    // selective-tone pull to reveal. `sorted[0]` is the least-affected channel,
                    // therefore this only activates once every channel is deeply into the final
                    // response shoulder; ordinary highlights and one-colour emitters stay out.
                    const float shared_terminal_risk = smoothstep(0.75F, 0.98F, sorted[0U]);
                    const float risk = std::max(disagreement_risk, shared_terminal_risk);
                    output.samples[
                        static_cast<std::size_t>(target_y) * target_dimensions.width + target_x
                    ] = static_cast<std::uint8_t>(std::lround(std::clamp(risk, 0.0F, 1.0F) * 255.0F));
                }
            }
        }
    );
    feather_terminal_highlight_chroma_boundaries(output);
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

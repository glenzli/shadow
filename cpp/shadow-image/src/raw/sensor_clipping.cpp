#include <shadow/image/sensor_clipping.hpp>

#include <shadow/image/decoder_error.hpp>

#include "../concurrency/row_scheduler.hpp"

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

} // namespace shadow::image

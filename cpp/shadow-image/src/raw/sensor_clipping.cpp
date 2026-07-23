#include <shadow/image/sensor_clipping.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::uint8_t observed_source_sample = 1U << 7U;

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

[[nodiscard]] Dimensions coordinate_in_display_orientation(
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
        return {y, active_dimensions.width - 1U - x};
    case 6: // LibRaw: 90° clockwise.
        return {active_dimensions.height - 1U - y, x};
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
    for (std::uint32_t active_y = 0U; active_y < descriptor.active_dimensions.height; ++active_y) {
        const std::uint32_t raw_y = descriptor.active_margins.top + active_y;
        for (std::uint32_t active_x = 0U; active_x < descriptor.active_dimensions.width; ++active_x) {
            const std::uint32_t raw_x = descriptor.active_margins.left + active_x;
            const Dimensions oriented = coordinate_in_display_orientation(
                descriptor.active_dimensions,
                descriptor.orientation,
                active_x,
                active_y
            );
            const auto target_x = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(oriented.width) * target_dimensions.width
                / oriented_active.width
            );
            const auto target_y = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(oriented.height) * target_dimensions.height
                / oriented_active.height
            );
            const auto target_index = static_cast<std::size_t>(target_y) * target_dimensions.width
                + target_x;
            auto& flags = output.samples[target_index];
            const auto site = cfa_site(raw_x, raw_y);
            const auto sample = frame.samples[static_cast<std::size_t>(raw_y) * storage_width + raw_x];
            const auto black = descriptor.black_levels[site];
            const auto white = descriptor.white_levels[site];

            if ((flags & observed_source_sample) == 0U) {
                flags = static_cast<std::uint8_t>(sensor_shadow_clipped | observed_source_sample);
            }
            if (sample > black) {
                flags = static_cast<std::uint8_t>(flags & ~sensor_shadow_clipped);
            }
            if (sample >= white) {
                flags = static_cast<std::uint8_t>(flags | sensor_highlight_clipped);
            }
        }
    }

    for (auto& flags : output.samples) {
        if ((flags & observed_source_sample) == 0U) {
            flags = 0U;
            continue;
        }
        flags = static_cast<std::uint8_t>(flags & ~observed_source_sample);
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

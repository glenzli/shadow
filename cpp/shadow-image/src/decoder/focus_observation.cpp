#include <shadow/image/focus_observation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace shadow::image {
namespace {

struct SourceFocusArea final {
    double center_x = 0.5;
    double center_y = 0.5;
    double width = 0.0;
    double height = 0.0;
};

[[nodiscard]] bool supported_orientation(const std::int32_t orientation) noexcept {
    return orientation == 0 || orientation == 1 || orientation == 3
        || orientation == 5 || orientation == 6;
}

[[nodiscard]] SourceFocusArea display_oriented(
    const SourceFocusArea source,
    const std::int32_t orientation
) noexcept {
    switch (orientation) {
    case 3:
        return {
            .center_x = 1.0 - source.center_x,
            .center_y = 1.0 - source.center_y,
            .width = source.width,
            .height = source.height,
        };
    case 5: // LibRaw: 90 degrees counterclockwise.
        return {
            .center_x = source.center_y,
            .center_y = 1.0 - source.center_x,
            .width = source.height,
            .height = source.width,
        };
    case 6: // LibRaw: 90 degrees clockwise.
        return {
            .center_x = 1.0 - source.center_y,
            .center_y = source.center_x,
            .width = source.height,
            .height = source.width,
        };
    case 0:
    case 1:
    default:
        return source;
    }
}

[[nodiscard]] bool normalized_area(const SourceFocusArea& area) noexcept {
    return std::isfinite(area.center_x) && std::isfinite(area.center_y)
        && std::isfinite(area.width) && std::isfinite(area.height)
        && area.center_x >= 0.0 && area.center_x <= 1.0
        && area.center_y >= 0.0 && area.center_y <= 1.0
        && area.width >= 0.0 && area.width <= 1.0
        && area.height >= 0.0 && area.height <= 1.0;
}

[[nodiscard]] std::optional<FocusObservation> observation(
    const SourceFocusArea source_area,
    const std::int32_t orientation,
    const FocusObservationSource source,
    const bool focus_confirmed,
    const double confidence
) noexcept {
    if (!supported_orientation(orientation) || !normalized_area(source_area)) {
        return std::nullopt;
    }
    const SourceFocusArea displayed = display_oriented(source_area, orientation);
    if (!normalized_area(displayed)) {
        return std::nullopt;
    }
    return FocusObservation{
        .schema_version = focus_observation_schema_version,
        .source = source,
        .center_x = std::clamp(displayed.center_x, 0.0, 1.0),
        .center_y = std::clamp(displayed.center_y, 0.0, 1.0),
        .width = displayed.width,
        .height = displayed.height,
        .focus_confirmed = focus_confirmed,
        .confidence = confidence,
    };
}

[[nodiscard]] std::optional<std::uint16_t> read_u16(
    const std::span<const std::uint8_t> record,
    const std::size_t offset,
    const FocusRecordByteOrder byte_order
) noexcept {
    if (offset > record.size() || record.size() - offset < 2U) {
        return std::nullopt;
    }
    const auto first = static_cast<std::uint16_t>(record[offset]);
    const auto second = static_cast<std::uint16_t>(record[offset + 1U]);
    return byte_order == FocusRecordByteOrder::little_endian
        ? static_cast<std::uint16_t>(first | (second << 8U))
        : static_cast<std::uint16_t>((first << 8U) | second);
}

[[nodiscard]] std::optional<FocusObservation> nikon_coordinate_tail(
    const std::span<const std::uint8_t> record,
    const std::size_t base_offset,
    const FocusRecordByteOrder byte_order,
    const std::int32_t libraw_orientation
) noexcept {
    if (base_offset > record.size() || record.size() - base_offset < 13U) {
        return std::nullopt;
    }
    const auto image_width = read_u16(record, base_offset, byte_order);
    const auto image_height = read_u16(record, base_offset + 2U, byte_order);
    const auto center_x = read_u16(record, base_offset + 4U, byte_order);
    const auto center_y = read_u16(record, base_offset + 6U, byte_order);
    const auto area_width = read_u16(record, base_offset + 8U, byte_order);
    const auto area_height = read_u16(record, base_offset + 10U, byte_order);
    if (!image_width.has_value() || !image_height.has_value()
        || !center_x.has_value() || !center_y.has_value()
        || !area_width.has_value() || !area_height.has_value()
        || *image_width == 0U || *image_height == 0U
        || *area_width == 0U || *area_height == 0U
        || *center_x > *image_width || *center_y > *image_height
        || *area_width > *image_width || *area_height > *image_height) {
        return std::nullopt;
    }
    const bool focus_confirmed = record[base_offset + 12U] == 1U;
    return observation(
        SourceFocusArea{
            .center_x = static_cast<double>(*center_x) / *image_width,
            .center_y = static_cast<double>(*center_y) / *image_height,
            .width = static_cast<double>(*area_width) / *image_width,
            .height = static_cast<double>(*area_height) / *image_height,
        },
        libraw_orientation,
        FocusObservationSource::camera_focus_area,
        focus_confirmed,
        focus_confirmed ? 1.0 : 0.7
    );
}

} // namespace

std::optional<FocusObservation> nikon_focus_observation(
    const std::uint32_t version,
    const FocusRecordByteOrder byte_order,
    const std::span<const std::uint8_t> record,
    const std::int32_t libraw_orientation
) noexcept {
    // AFInfo2 V0400, V0401 and V0402 share the Expeed 7 coordinate tail. The
    // public MakerNote record uses offset 62; LibRaw 0.22's retained
    // AFInfoData omits a two-byte leading element and presents that tail at
    // offset 60. This API consumes LibRaw's retained payload, never raw TIFF
    // bytes, so the shifted layout is the only admitted contract.
    if (version < 400U || version > 402U) {
        return std::nullopt;
    }
    return nikon_coordinate_tail(record, 60U, byte_order, libraw_orientation);
}

std::optional<FocusObservation> sony_focus_observation(
    const std::array<std::uint16_t, 4U>& focus_location,
    const std::int32_t libraw_orientation
) noexcept {
    const std::uint16_t image_width = focus_location[0U];
    const std::uint16_t image_height = focus_location[1U];
    const std::uint16_t center_x = focus_location[2U];
    const std::uint16_t center_y = focus_location[3U];
    if (image_width == 0U || image_height == 0U
        || image_width == 0xffffU || image_height == 0xffffU
        || center_x > image_width
        || center_y > image_height) {
        return std::nullopt;
    }
    return observation(
        SourceFocusArea{
            .center_x = static_cast<double>(center_x) / image_width,
            .center_y = static_cast<double>(center_y) / image_height,
        },
        libraw_orientation,
        FocusObservationSource::camera_focus_location,
        false,
        0.85
    );
}

std::string_view to_string(const FocusObservationSource source) noexcept {
    switch (source) {
    case FocusObservationSource::camera_focus_area:
        return "camera-focus-area";
    case FocusObservationSource::camera_focus_location:
        return "camera-focus-location";
    case FocusObservationSource::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace shadow::image

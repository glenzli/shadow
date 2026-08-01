#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace shadow::image {

inline constexpr std::uint32_t focus_observation_schema_version = 1U;

/// Provenance of one camera-authored focus location. The values describe what
/// the source established, not whether the photographed subject is sharp.
enum class FocusObservationSource : std::uint8_t {
    unknown,
    camera_focus_area,
    camera_focus_location,
};

/// One bounded focus point or area in the display-oriented, uncropped source
/// coordinate space. Width and height are zero when the camera supplies only a
/// point. Geometry and Liquify remain later view transforms.
struct FocusObservation final {
    std::uint32_t schema_version = focus_observation_schema_version;
    FocusObservationSource source = FocusObservationSource::unknown;
    double center_x = 0.5;
    double center_y = 0.5;
    double width = 0.0;
    double height = 0.0;
    bool focus_confirmed = false;
    double confidence = 0.0;

    auto operator<=>(const FocusObservation&) const = default;
};

enum class FocusRecordByteOrder : std::uint8_t {
    little_endian,
    big_endian,
};

/// Decodes the public Nikon AFInfo2 V0400-family coordinate record retained by
/// LibRaw. Unsupported versions or malformed/incomplete records return no
/// observation rather than guessing a focus point.
[[nodiscard]] std::optional<FocusObservation> nikon_focus_observation(
    std::uint32_t version,
    FocusRecordByteOrder byte_order,
    std::span<const std::uint8_t> record,
    std::int32_t libraw_orientation
) noexcept;

/// Converts LibRaw's Sony FocusLocation tuple `{AF image width, AF image
/// height, x, y}` into the same display-oriented source coordinate space.
[[nodiscard]] std::optional<FocusObservation> sony_focus_observation(
    const std::array<std::uint16_t, 4U>& focus_location,
    std::int32_t libraw_orientation
) noexcept;

[[nodiscard]] std::string_view to_string(FocusObservationSource source) noexcept;

} // namespace shadow::image

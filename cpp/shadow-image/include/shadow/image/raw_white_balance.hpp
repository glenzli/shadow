#pragma once

#include <shadow/image/camera_profile.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <optional>

namespace shadow::image {

// Human-facing presentation of one absolute RAW white point. `tint` follows
// the photographic convention: negative is greener and positive is more
// magenta. The xy fields retain the exact white point used by the native
// calibration solve.
struct RawWhiteBalancePresentation final {
    double temperature_kelvin = 0.0;
    double tint = 0.0;
    double white_x = 0.0;
    double white_y = 0.0;

    [[nodiscard]] bool valid() const noexcept;
};

// Converts the authored photographic controls to a CIE xy white point and
// back. One tint unit is a 0.0001 displacement from the temperature locus in
// CIE 1960 UCS; this keeps the public -150..150 scale useful without exposing
// camera-channel ratios.
[[nodiscard]] std::optional<std::array<double, 2U>>
raw_white_xy_from_temperature_tint(double temperature_kelvin, double tint) noexcept;

[[nodiscard]] std::optional<RawWhiteBalancePresentation>
raw_white_balance_presentation_from_xy(double white_x, double white_y) noexcept;

// Resolves source metadata or authored temperature/tint into the
// green-normalized camera neutral consumed by RAW development.
[[nodiscard]] std::optional<std::array<double, 3U>>
raw_as_shot_camera_neutral(const RawFrameDescriptor& descriptor) noexcept;

[[nodiscard]] std::optional<std::array<double, 3U>> raw_dcp_camera_neutral(
    const DcpProfile& profile,
    const RawWhiteBalance& white_balance
) noexcept;

[[nodiscard]] std::optional<RawWhiteBalancePresentation>
raw_dcp_white_balance_presentation(
    const DcpProfile& profile,
    const std::array<double, 3U>& camera_neutral
) noexcept;

[[nodiscard]] std::optional<std::array<double, 3U>> raw_frame_camera_neutral(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) noexcept;

} // namespace shadow::image

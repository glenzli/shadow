#pragma once

#include <cstdint>

namespace shadow::image::display_rgb {

inline constexpr std::uint32_t rec709_red_q16 = 13'933U;
inline constexpr std::uint32_t rec709_green_q16 = 46'871U;
inline constexpr std::uint32_t rec709_blue_q16 = 4'732U;
inline constexpr std::uint32_t q16_one = 65'536U;
inline constexpr std::uint32_t maximum_weighted_luma = 255U * q16_one;

static_assert(rec709_red_q16 + rec709_green_q16 + rec709_blue_q16 == q16_one);
static_assert(maximum_weighted_luma < (1U << 24U));

[[nodiscard]] constexpr std::uint32_t rec709_encoded_luma_q16(
    const std::uint8_t red,
    const std::uint8_t green,
    const std::uint8_t blue
) noexcept {
    return rec709_red_q16 * red + rec709_green_q16 * green + rec709_blue_q16 * blue;
}

[[nodiscard]] constexpr std::uint8_t rec709_encoded_luma_u8(
    const std::uint8_t red,
    const std::uint8_t green,
    const std::uint8_t blue
) noexcept {
    return static_cast<std::uint8_t>(
        (rec709_encoded_luma_q16(red, green, blue) + q16_one / 2U) / q16_one
    );
}

} // namespace shadow::image::display_rgb

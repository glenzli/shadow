#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace shadow::image {

// All of LibRaw's user-visible processing switches are concentrated here rather than being
// spread across preview and detail render code. This is the configuration boundary between
// Shadow's provider-neutral decoder contract and LibRaw's private processing API. It deliberately
// describes the reference/development raster only; sensor-domain RAW white balance, DNG opcodes,
// optical profiles and camera-specific colour transforms are separate pipeline stages.
//
// `demosaic_quality` maps directly to LibRaw's documented `user_qual` selector. It remains an
// implementation detail because the available algorithms vary with the linked LibRaw build.
struct LibRawDevelopmentSettings final {
    std::uint32_t schema_version = 1;
    bool use_camera_white_balance = true;
    bool use_camera_matrix = true;
    bool use_auto_brightness = false;
    bool use_exposure_correction = false;
    float brightness = 1.0F;
    float maximum_adjustment_threshold = 0.0F;
    std::uint16_t output_bits_per_channel = 16;
    std::int32_t demosaic_quality = 3;

    auto operator<=>(const LibRawDevelopmentSettings&) const = default;
};

inline constexpr std::uint32_t libraw_development_settings_schema_version = 1U;

[[nodiscard]] LibRawDevelopmentSettings default_libraw_development_settings() noexcept;
[[nodiscard]] std::string libraw_development_settings_signature(
    const LibRawDevelopmentSettings& settings
);

} // namespace shadow::image

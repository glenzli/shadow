#pragma once

#include <shadow/image/raw_denoise.hpp>

#include "metal_raw_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace shadow::image::detail {

struct MetalRawDenoiseParameters final {
    std::uint32_t storage_width = 0U;
    std::uint32_t storage_height = 0U;
    std::uint32_t active_left = 0U;
    std::uint32_t active_top = 0U;
    std::uint32_t active_right = 0U;
    std::uint32_t active_bottom = 0U;
    std::uint32_t mode = 0U;
    std::uint32_t uses_calibrated_sensor_noise = 0U;
    float iso_sensitivity = 0.0F;
    std::array<float, 4U> black_levels{};
    std::array<float, 4U> white_levels{};
    std::array<float, 4U> read_noise_stddev_dn{};
    std::array<float, 4U> shot_noise_variance_per_dn{};
};

static_assert(sizeof(MetalRawDenoiseParameters) == 100U);
static_assert(offsetof(MetalRawDenoiseParameters, storage_width) == 0U);
static_assert(offsetof(MetalRawDenoiseParameters, mode) == 24U);
static_assert(offsetof(MetalRawDenoiseParameters, iso_sensitivity) == 32U);
static_assert(offsetof(MetalRawDenoiseParameters, black_levels) == 36U);
static_assert(offsetof(MetalRawDenoiseParameters, white_levels) == 52U);
static_assert(offsetof(MetalRawDenoiseParameters, read_noise_stddev_dn) == 68U);
static_assert(offsetof(MetalRawDenoiseParameters, shot_noise_variance_per_dn) == 84U);

// Owns the mirrored denoise ABI and the source-copy + same-CFA compute encoding. The destination
// must begin as an exact source copy because inactive margins are intentionally left untouched;
// encoding that copy on Metal avoids a second host upload in resident RAW development.
class MetalRawDenoiseEncoding final {
  public:
    [[nodiscard]] static std::optional<MetalRawDenoiseEncoding> prepare(
        const RawFrame& frame,
        RawBayerDenoiseMode mode,
        double iso_sensitivity,
        std::string& diagnostic
    );

    [[nodiscard]] std::size_t sample_bytes() const noexcept;
    [[nodiscard]] bool encode(
        id<MTLCommandBuffer> command_buffer,
        id<MTLBuffer> source,
        id<MTLBuffer> destination,
        std::string& diagnostic
    ) const;

  private:
    MetalRawDenoiseParameters parameters_;
    std::size_t sample_bytes_ = 0U;
};

} // namespace shadow::image::detail

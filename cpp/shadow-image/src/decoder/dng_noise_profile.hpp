#pragma once

#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace shadow::image {

inline constexpr std::uint32_t dng_noise_profile_contract_version = 1U;

// Exact, normalized DNG NoiseProfile coefficients in canonical R, G, B order.
// The DNG model is Var(x) = scale * x + offset for a black-subtracted linear
// signal x in [0, 1]. This remains separate from RawFrame's DN-unit model until
// the decoder has resolved the actual per-site black and white levels.
struct DngNoiseProfile final {
    std::array<double, 3U> normalized_scale{};
    std::array<double, 3U> normalized_offset{};

    [[nodiscard]] bool valid() const noexcept;
};

enum class DngNoiseProfileStatus : std::uint8_t {
    exact,
    not_dng,
    absent,
    malformed,
    unsupported_tiff_layout,
    unsupported_profile_shape,
    ambiguous_raw_ifd,
    io_error,
};

struct DngNoiseProfileReceipt final {
    DngNoiseProfileStatus status = DngNoiseProfileStatus::not_dng;
    DngNoiseProfile profile;
    std::uint32_t ifds_visited = 0U;

    [[nodiscard]] bool exact() const noexcept;
};

// Parses only TIFF/DNG metadata from a bounded byte span. Pixel payloads are
// never decoded or copied.
[[nodiscard]] DngNoiseProfileReceipt
parse_dng_noise_profile(std::span<const std::uint8_t> bytes) noexcept;

// Reads the same bounded metadata fields through random file access, so a
// large RAW payload is not loaded merely to inspect its noise calibration.
[[nodiscard]] DngNoiseProfileReceipt
read_dng_noise_profile(const std::filesystem::path& path) noexcept;

// Converts the normalized DNG model to RawFrame's per-CFA-site DN units:
//   Var(DN) = (scale * range) * (DN - black) + offset * range^2.
// Unsupported or incomplete inputs return the canonical unavailable model.
[[nodiscard]] RawSensorNoiseCalibration resolve_dng_noise_profile(
    const DngNoiseProfileReceipt& receipt,
    const RawFrameDescriptor& descriptor,
    double iso_sensitivity
) noexcept;

} // namespace shadow::image

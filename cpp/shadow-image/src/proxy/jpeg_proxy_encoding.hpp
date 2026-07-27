#pragma once

#include <shadow/image/decoder_types.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace shadow::image::proxy_detail {

void validate_jpeg_quality(std::uint8_t jpeg_quality);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_proxy_jpeg_cancellable(
    std::span<const std::uint8_t> rgb,
    Dimensions dimensions,
    std::uint8_t quality,
    std::stop_token cancellation
);

[[nodiscard]] std::vector<std::uint8_t> encode_proxy_jpeg(
    std::span<const std::uint8_t> rgb,
    Dimensions dimensions,
    std::uint8_t quality
);

} // namespace shadow::image::proxy_detail

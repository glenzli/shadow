#pragma once

#include <shadow/image/decoder_types.hpp>

#include <cstdint>
#include <vector>

namespace shadow::image {

class DecodeSession;

struct ProxyRequest final {
    std::uint32_t max_edge = 2'048;
    std::uint8_t jpeg_quality = 95;
};

struct EncodedProxy final {
    Dimensions dimensions;
    PreviewFormat format = PreviewFormat::jpeg;
    std::uint16_t bits_per_channel = 8;
    std::uint16_t channels = 3;
    std::vector<std::uint8_t> bytes;
};

[[nodiscard]] Dimensions proxy_dimensions(Dimensions source, std::uint32_t max_edge);

[[nodiscard]] EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    ProxyRequest request = {}
);

} // namespace shadow::image

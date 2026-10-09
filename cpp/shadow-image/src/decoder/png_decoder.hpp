#pragma once

#include <shadow/image/decoder_session.hpp>

namespace shadow::image {
// Deliberately bounded rendered-raster contract: 8-bit, opaque sRGB PNG only.
[[nodiscard]] std::unique_ptr<DecodeSession>
open_png_decode_session(const std::filesystem::path& path);
[[nodiscard]] std::string png_decoder_version();
} // namespace shadow::image

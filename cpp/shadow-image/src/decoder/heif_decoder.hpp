#pragma once

#include <shadow/image/decoder_session.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace shadow::image {

// These functions deliberately form a small optional-backend seam.  The public raster provider
// remains available in every build; a build without libheif reports an explicit unsupported
// error only when a HEIF-family file is opened.
[[nodiscard]] bool heif_decoder_available() noexcept;
[[nodiscard]] std::string heif_decoder_version();
[[nodiscard]] std::unique_ptr<DecodeSession> open_heif_decode_session(
    const std::filesystem::path& path
);

} // namespace shadow::image

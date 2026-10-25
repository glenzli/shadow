#pragma once

#include <filesystem>
#include <string_view>

namespace shadow::image {
class DecodeSession;
}

int write_embedded_preview(
    shadow::image::DecodeSession& session,
    const std::filesystem::path& output,
    std::string_view nonce
);

// Camera JPEG bytes are copied in the isolated helper; this operation never develops RAW.
int extract_embedded_preview(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::string_view nonce
);

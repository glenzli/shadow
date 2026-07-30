#pragma once

#include <shadow/image/raw_frame.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace shadow::image {

inline constexpr std::string_view raw_frame_staging_schema =
    "shadow-raw-frame-staging-v1";

struct RawFrameStagingReceipt final {
    std::filesystem::path manifest_path;
    std::filesystem::path sample_path;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint64_t sample_bytes = 0U;
};

/// Publishes one provider-neutral active Bayer plane for a short-lived local
/// AI sidecar transaction. The manifest is published last, so its presence
/// always implies a complete little-endian uint16 sample file.
[[nodiscard]] RawFrameStagingReceipt write_raw_frame_staging(
    const RawFrame& frame,
    const std::filesystem::path& manifest_path,
    std::string_view nonce
);

} // namespace shadow::image

#pragma once

#include <shadow/image/decoder_types.hpp>

#include <libraw/libraw.h>

#include <filesystem>
#include <string_view>

namespace shadow::image {

[[noreturn]] void throw_libraw_error(
    int result,
    std::string_view operation
);

void require_libraw_success(
    int result,
    std::string_view operation
);

[[nodiscard]] int libraw_open_path(
    LibRaw& decoder,
    const std::filesystem::path& path
);

[[nodiscard]] PendingCorrections libraw_pending_corrections(
    const libraw_data_t& data
) noexcept;

} // namespace shadow::image

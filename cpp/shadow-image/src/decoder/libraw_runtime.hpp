#pragma once

#include <shadow/image/decoder_types.hpp>

#include <libraw/libraw.h>

#include <filesystem>
#include <string_view>

namespace shadow::image {

// Optional public DNG SDK host. Declare it before its LibRaw instance so LibRaw is
// destroyed first; the SDK host never crosses the provider-neutral image API.
class LibRawDngHost final {
  public:
    LibRawDngHost();
    ~LibRawDngHost();
    LibRawDngHost(const LibRawDngHost&) = delete;
    LibRawDngHost& operator=(const LibRawDngHost&) = delete;
    // Call after open_file: only an identified JPEG XL DNG receives the SDK host.
    void attach(LibRaw& decoder) const noexcept;
    [[nodiscard]] bool available() const noexcept { return host_ != nullptr; }

  private:
    void* host_ = nullptr;
};

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

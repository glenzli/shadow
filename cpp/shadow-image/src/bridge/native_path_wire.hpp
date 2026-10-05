#pragma once

#include "rust/cxx.h"

#include <cstdint>
#include <filesystem>

namespace shadow::bridge::native_path_wire {

// Compatibility text route. Windows continues to use its existing UTF-8 boundary.
[[nodiscard]] std::filesystem::path from_utf8(rust::Str path);
// Unix OsStr bytes only; never transcode through UTF-8 or a locale.
[[nodiscard]] std::filesystem::path from_unix_bytes(rust::Slice<const std::uint8_t> path);

} // namespace shadow::bridge::native_path_wire

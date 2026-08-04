#pragma once

#include "library_server_types.hpp"

namespace shadow::desktop {
struct FfiLibraryServerConfig;
struct FfiLibraryServerSnapshot;
} // namespace shadow::desktop

namespace library_server_projection {

[[nodiscard]] BackendLibraryServerSnapshot
snapshot(const shadow::desktop::FfiLibraryServerSnapshot& source);

[[nodiscard]] shadow::desktop::FfiLibraryServerConfig
config(const BackendLibraryServerConfig& source);

} // namespace library_server_projection

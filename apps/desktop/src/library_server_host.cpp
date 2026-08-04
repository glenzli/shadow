#include "library_server_host.hpp"

#include "backend/library_server_projection.hpp"
#include "shadow-desktop-bridge/src/lib.rs.h"

#include <stdexcept>

struct LibraryServerHost::Impl final {
    explicit Impl(const QString& storage_root) :
        host(shadow::desktop::open_library_server_host(storage_root.toStdString())) {}

    rust::Box<shadow::desktop::LibraryServerHost> host;
};

LibraryServerHost::LibraryServerHost(const QString& storage_root) :
    impl_(std::make_unique<Impl>(storage_root)) {}

LibraryServerHost::~LibraryServerHost() = default;

LibraryServerControllerOperations LibraryServerHost::operations() {
    return {
        .snapshot = [this] { return library_server_projection::snapshot(impl_->host->snapshot()); },
        .start =
            [this](const BackendLibraryServerConfig& config) {
                const auto ffi_config = library_server_projection::config(config);
                return library_server_projection::snapshot(impl_->host->start(ffi_config));
            },
        .stop = [this] { return library_server_projection::snapshot(impl_->host->stop()); },
        .reset_cache =
            [this] { return library_server_projection::snapshot(impl_->host->reset_cache()); },
    };
}

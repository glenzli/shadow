#include "backend/desktop_backend_private.hpp"
#include "backend/library_server_projection.hpp"

BackendLibraryServerSnapshot DesktopBackend::libraryServerSnapshot() const {
    return library_server_projection::snapshot(impl_->session->library_server_snapshot());
}

BackendLibraryServerSnapshot
DesktopBackend::startLibraryServer(const BackendLibraryServerConfig& config) const {
    const auto ffi_config = library_server_projection::config(config);
    return library_server_projection::snapshot(impl_->session->start_library_server(ffi_config));
}

BackendLibraryServerSnapshot DesktopBackend::stopLibraryServer() const {
    return library_server_projection::snapshot(impl_->session->stop_library_server());
}

BackendLibraryServerSnapshot DesktopBackend::resetLibraryServerCache() const {
    return library_server_projection::snapshot(impl_->session->reset_library_server_cache());
}

#include "backend/desktop_backend_private.hpp"
#include "backend/library_server_types.hpp"
#include "backend/rust_qt_projection.hpp"

namespace {

using desktop_backend_projection::qstring;

[[nodiscard]] BackendLibraryServerSnapshot
projectSnapshot(const shadow::desktop::FfiLibraryServerSnapshot& source) {
    return {
        .running = source.running,
        .local_address = qstring(source.local_address),
        .display_name = qstring(source.display_name),
        .provider_mode = qstring(source.provider_mode),
        .photo_count = source.photo_count,
        .cache_byte_len = source.cache_byte_len,
        .shared_root_count = source.shared_root_count,
        .serves_originals = source.serves_originals,
    };
}

[[nodiscard]] shadow::desktop::FfiLibraryServerConfig
ffiConfig(const BackendLibraryServerConfig& source) {
    shadow::desktop::FfiLibraryServerConfig result;
    result.bind_address = source.bind_address.toStdString();
    result.authorization = source.authorization.toStdString();
    result.display_name = source.display_name.toStdString();
    result.serves_originals = source.serves_originals;
    result.share_roots.reserve(static_cast<std::size_t>(source.share_roots.size()));
    for (const QString& root : source.share_roots) {
        result.share_roots.push_back(root.toStdString());
    }
    return result;
}

} // namespace

BackendLibraryServerSnapshot DesktopBackend::libraryServerSnapshot() const {
    return projectSnapshot(impl_->session->library_server_snapshot());
}

BackendLibraryServerSnapshot
DesktopBackend::startLibraryServer(const BackendLibraryServerConfig& config) const {
    const auto ffi_config = ffiConfig(config);
    return projectSnapshot(impl_->session->start_library_server(ffi_config));
}

BackendLibraryServerSnapshot DesktopBackend::stopLibraryServer() const {
    return projectSnapshot(impl_->session->stop_library_server());
}

BackendLibraryServerSnapshot DesktopBackend::resetLibraryServerCache() const {
    return projectSnapshot(impl_->session->reset_library_server_cache());
}

#include "library_server_projection.hpp"

#include "backend/rust_qt_projection.hpp"
#include "shadow-desktop-bridge/src/lib.rs.h"

namespace library_server_projection {

using desktop_backend_projection::qstring;

BackendLibraryServerSnapshot snapshot(const shadow::desktop::FfiLibraryServerSnapshot& source) {
    return {
        .running = source.running,
        .local_address = qstring(source.local_address),
        .display_name = qstring(source.display_name),
        .provider_mode = qstring(source.provider_mode),
        .photo_count = source.photo_count,
        .cache_byte_len = source.cache_byte_len,
        .shared_root_count = source.shared_root_count,
        .serves_originals = source.serves_originals,
        .index_state = qstring(source.index_state),
        .discovered_file_count = source.discovered_file_count,
        .inspection_completed_count = source.inspection_completed_count,
        .published_preview_count = source.published_preview_count,
        .index_diagnostic = qstring(source.index_diagnostic),
    };
}

shadow::desktop::FfiLibraryServerConfig config(const BackendLibraryServerConfig& source) {
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

} // namespace library_server_projection

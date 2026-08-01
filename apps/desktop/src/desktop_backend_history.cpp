#include "backend/desktop_backend_private.hpp"
#include "backend/history_projection.hpp"

BackendPhotoHistoryPage DesktopBackend::photoHistoryPage(
    const QString& photo_id,
    const BackendHistoryCursor& after,
    const std::uint32_t limit
) const {
    const auto cursor = desktop_backend_projection::ffi_history_cursor(after);
    return desktop_backend_projection::photo_history_page(
        impl_->session->photo_edit_history_page(photo_id.toStdString(), cursor, limit)
    );
}

BackendLibraryHistoryPage DesktopBackend::libraryHistoryPage(
    const BackendHistoryCursor& after,
    const std::uint32_t limit
) const {
    const auto cursor = desktop_backend_projection::ffi_history_cursor(after);
    return desktop_backend_projection::library_history_page(
        impl_->session->library_edit_history_page(cursor, limit)
    );
}

BackendLibraryHistoryRefPage
DesktopBackend::libraryHistoryRefPage(const QString& after_name, const std::uint32_t limit) const {
    return desktop_backend_projection::library_history_ref_page(
        impl_->session->library_edit_history_ref_page(after_name.toStdString(), limit)
    );
}

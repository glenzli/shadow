#include "backend/desktop_backend_private.hpp"
#include "photo_inspection_projection.hpp"

#include <memory>
#include <utility>

DesktopBackend::DesktopBackend(const QString& catalog_path, const QString& cache_root)
    : impl_(std::make_unique<Impl>(shadow::desktop::open_desktop_session(
          catalog_path.toStdString(),
          cache_root.toStdString()
      ))) {}

DesktopBackend::~DesktopBackend() = default;

void DesktopBackend::beginFolderScan(const std::uint64_t scan_id) const {
    impl_->folder_scan_backend.beginFolderScan(scan_id);
}

BackendScanReport DesktopBackend::scanFolder(
    const QString& folder_path,
    const std::uint64_t scan_id
) const {
    return impl_->folder_scan_backend.scanFolder(folder_path, scan_id);
}

BackendScanProgress DesktopBackend::scanProgress(const std::uint64_t scan_id) const {
    return impl_->folder_scan_backend.scanProgress(scan_id);
}

bool DesktopBackend::cancelFolderScan(const std::uint64_t scan_id) const {
    return impl_->folder_scan_backend.cancelFolderScan(scan_id);
}

BackendPhotoInspection DesktopBackend::photoInspection(
    const QString& photo_id,
    const QString& representation_id
) const {
    const auto source = impl_->session->photo_inspection(
        photo_id.toStdString(),
        representation_id.toStdString()
    );
    return project_photo_inspection(source);
}

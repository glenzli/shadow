#include "backend/desktop_backend_private.hpp"
#include "backend/remote_library_types.hpp"
#include "backend/review_projection.hpp"
#include "backend/rust_qt_projection.hpp"

namespace {

using desktop_backend_projection::checked_qt_vector_size;
using desktop_backend_projection::decision_flag;
using desktop_backend_projection::ffi_decision_flag;
using desktop_backend_projection::qstring;

[[nodiscard]] BackendRemoteLibraryServer
project_server(const shadow::desktop::FfiRemoteLibraryServer& source) {
    return {
        .server_id = qstring(source.server_id),
        .display_name = qstring(source.display_name),
        .embedded_previews_available = source.embedded_previews_available,
        .generated_proxies_available = source.generated_proxies_available,
        .originals_available = source.originals_available,
        .private_preview_provider_available = source.private_preview_provider_available,
    };
}

[[nodiscard]] BackendRemoteLibraryPhoto
project_photo(const shadow::desktop::FfiRemoteLibraryPhoto& source) {
    return {
        .server_id = qstring(source.server_id),
        .remote_photo_id = qstring(source.remote_photo_id),
        .remote_representation_id = qstring(source.remote_representation_id),
        .title = qstring(source.title),
        .source_byte_len = source.source_byte_len,
        .has_source_modified_at = source.has_source_modified_at,
        .source_modified_at_ms = source.source_modified_at_ms,
        .has_original_identity = source.has_original_identity,
        .original_digest_hex = qstring(source.original_digest_hex),
        .representation_count = source.representation_count,
        .source_location_count = source.source_location_count,
        .has_raw_representation = source.has_raw_representation,
        .has_raster_representation = source.has_raster_representation,
        .has_preview = source.has_preview,
        .preview_path = qstring(source.preview_path),
        .preview_role = qstring(source.preview_role),
        .preview_width = source.preview_width,
        .preview_height = source.preview_height,
        .preview_unavailable_reason = qstring(source.preview_unavailable_reason),
        .has_captured_at = source.has_captured_at,
        .captured_at_unix_seconds = source.captured_at_unix_seconds,
        .camera_make = qstring(source.camera_make),
        .camera_model = qstring(source.camera_model),
        .lens_make = qstring(source.lens_make),
        .lens_model = qstring(source.lens_model),
        .has_iso_speed = source.has_iso_speed,
        .iso_speed = source.iso_speed,
        .has_exposure_time = source.has_exposure_time,
        .exposure_time_seconds = source.exposure_time_seconds,
        .has_aperture = source.has_aperture,
        .aperture_f_number = source.aperture_f_number,
        .has_focal_length = source.has_focal_length,
        .focal_length_mm = source.focal_length_mm,
        .has_raw_dimensions = source.has_raw_dimensions,
        .raw_width = source.raw_width,
        .raw_height = source.raw_height,
        .decision_flag = decision_flag(source.decision_flag),
        .decision_rating = source.decision_rating,
        .liked = source.liked,
        .color_label = qstring(source.color_label),
        .review_updated_at_ms = source.review_updated_at_ms,
        .is_materialized = source.is_materialized,
        .local_photo_id = qstring(source.local_photo_id),
        .local_representation_id = qstring(source.local_representation_id),
        .local_source_path = qstring(source.local_source_path),
    };
}

[[nodiscard]] BackendRemoteLibrarySnapshot
project_snapshot(const shadow::desktop::FfiRemoteLibrarySnapshot& source) {
    BackendRemoteLibrarySnapshot result{
        .has_server = source.has_server,
        .server = project_server(source.server),
    };
    result.photos.reserve(checked_qt_vector_size(source.photos.size(), "remote Library photos"));
    for (const auto& photo : source.photos) {
        result.photos.push_back(project_photo(photo));
    }
    return result;
}

} // namespace

BackendRemoteLibrarySnapshot
DesktopBackend::remoteLibrarySnapshot(const QString& connection_id) const {
    return project_snapshot(impl_->session->remote_library_snapshot(connection_id.toStdString()));
}

BackendRemoteLibrarySyncResult DesktopBackend::syncRemoteLibrary(
    const QString& connection_id,
    const QString& server_address,
    const QString& authorization
) const {
    const auto result = impl_->session->sync_remote_library(
        connection_id.toStdString(),
        server_address.toStdString(),
        authorization.toStdString()
    );
    return {
        .snapshot = project_snapshot(result.snapshot),
        .page_count = result.page_count,
        .photo_count = result.photo_count,
        .downloaded_previews = result.downloaded_previews,
        .removed = result.removed,
    };
}

void DesktopBackend::setRemoteLibraryReviewState(
    const QString& connection_id,
    const QString& remote_photo_id,
    const QString& remote_representation_id,
    const BackendReviewDecisionFlag flag,
    const std::uint8_t rating,
    const bool liked,
    const QString& color_label,
    const std::int64_t updated_at_ms
) const {
    impl_->session->set_remote_library_review_state(
        connection_id.toStdString(),
        remote_photo_id.toStdString(),
        remote_representation_id.toStdString(),
        ffi_decision_flag(flag),
        rating,
        liked,
        color_label.toStdString(),
        updated_at_ms
    );
}

BackendRemoteLibraryMaterialization DesktopBackend::materializeRemoteLibraryPhoto(
    const QString& connection_id,
    const QString& server_address,
    const QString& authorization,
    const QString& remote_photo_id,
    const QString& remote_representation_id
) const {
    const auto result = impl_->session->materialize_remote_library_photo(
        connection_id.toStdString(),
        server_address.toStdString(),
        authorization.toStdString(),
        remote_photo_id.toStdString(),
        remote_representation_id.toStdString()
    );
    return {
        .local_photo_id = qstring(result.local_photo_id),
        .local_representation_id = qstring(result.local_representation_id),
        .local_source_path = qstring(result.local_source_path),
        .title = qstring(result.title),
        .reused_existing = result.reused_existing,
    };
}

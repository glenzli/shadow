#include "backend/desktop_backend_private.hpp"
#include "backend/review_projection.hpp"
#include "backend/rust_qt_projection.hpp"

#include <stdexcept>

namespace {

using desktop_backend_projection::checked_qt_vector_size;
using desktop_backend_projection::decision_flag;
using desktop_backend_projection::qstring;

[[nodiscard]] shadow::desktop::FfiLibraryFlagFilter ffi_library_flag(
    const BackendLibraryFlagFilter flag
) {
    switch (flag) {
    case BackendLibraryFlagFilter::Any:
        return shadow::desktop::FfiLibraryFlagFilter::Any;
    case BackendLibraryFlagFilter::Unflagged:
        return shadow::desktop::FfiLibraryFlagFilter::Unflagged;
    case BackendLibraryFlagFilter::Picked:
        return shadow::desktop::FfiLibraryFlagFilter::Picked;
    case BackendLibraryFlagFilter::Rejected:
        return shadow::desktop::FfiLibraryFlagFilter::Rejected;
    }
    throw std::invalid_argument("unknown Library flag filter");
}

[[nodiscard]] shadow::desktop::FfiLibraryPhotoFilter ffi_library_filter(
    const BackendLibraryPhotoFilter& source
) {
    shadow::desktop::FfiLibraryPhotoFilter filter;
    filter.has_capture_start = source.has_capture_start;
    filter.capture_start_unix_seconds = source.capture_start_unix_seconds;
    filter.has_capture_end = source.has_capture_end;
    filter.capture_end_unix_seconds = source.capture_end_unix_seconds;
    filter.capture_month = source.capture_month.toStdString();
    filter.camera_key = source.camera_key.toStdString();
    filter.lens_key = source.lens_key.toStdString();
    filter.has_aperture_minimum = source.has_aperture_minimum;
    filter.aperture_minimum_milli = source.aperture_minimum_milli;
    filter.has_aperture_maximum = source.has_aperture_maximum;
    filter.aperture_maximum_milli = source.aperture_maximum_milli;
    filter.has_liked = source.has_liked;
    filter.liked = source.liked;
    filter.color_label = source.color_label.toStdString();
    filter.flag = ffi_library_flag(source.flag);
    filter.has_minimum_rating = source.has_minimum_rating;
    filter.minimum_rating = source.minimum_rating;
    filter.has_development_edits = source.has_development_edits;
    filter.development_edits = source.development_edits;
    filter.album_id = source.album_id.toStdString();
    return filter;
}

[[nodiscard]] BackendLibraryFlagFilter library_flag_filter(
    const shadow::desktop::FfiLibraryFlagFilter source
) {
    switch (source) {
    case shadow::desktop::FfiLibraryFlagFilter::Any:
        return BackendLibraryFlagFilter::Any;
    case shadow::desktop::FfiLibraryFlagFilter::Unflagged:
        return BackendLibraryFlagFilter::Unflagged;
    case shadow::desktop::FfiLibraryFlagFilter::Picked:
        return BackendLibraryFlagFilter::Picked;
    case shadow::desktop::FfiLibraryFlagFilter::Rejected:
        return BackendLibraryFlagFilter::Rejected;
    }
    throw std::invalid_argument("unknown Library flag filter");
}

[[nodiscard]] BackendLibraryPhotoFilter library_filter(
    const shadow::desktop::FfiLibraryPhotoFilter& source
) {
    return {
        .has_capture_start = source.has_capture_start,
        .capture_start_unix_seconds = source.capture_start_unix_seconds,
        .has_capture_end = source.has_capture_end,
        .capture_end_unix_seconds = source.capture_end_unix_seconds,
        .capture_month = qstring(source.capture_month),
        .camera_key = qstring(source.camera_key),
        .lens_key = qstring(source.lens_key),
        .has_aperture_minimum = source.has_aperture_minimum,
        .aperture_minimum_milli = source.aperture_minimum_milli,
        .has_aperture_maximum = source.has_aperture_maximum,
        .aperture_maximum_milli = source.aperture_maximum_milli,
        .has_liked = source.has_liked,
        .liked = source.liked,
        .color_label = qstring(source.color_label),
        .flag = library_flag_filter(source.flag),
        .has_minimum_rating = source.has_minimum_rating,
        .minimum_rating = source.minimum_rating,
        .has_development_edits = source.has_development_edits,
        .development_edits = source.development_edits,
        .album_id = qstring(source.album_id),
    };
}

[[nodiscard]] shadow::desktop::FfiLibraryFacetKind ffi_library_facet_kind(
    const BackendLibraryFacetKind kind
) {
    switch (kind) {
    case BackendLibraryFacetKind::CaptureMonth:
        return shadow::desktop::FfiLibraryFacetKind::CaptureMonth;
    case BackendLibraryFacetKind::Camera:
        return shadow::desktop::FfiLibraryFacetKind::Camera;
    case BackendLibraryFacetKind::Lens:
        return shadow::desktop::FfiLibraryFacetKind::Lens;
    }
    throw std::invalid_argument("unknown Library facet kind");
}

[[nodiscard]] shadow::desktop::FfiLibraryFacetCursor ffi_library_facet_cursor(
    const BackendLibraryFacetCursor& source
) {
    shadow::desktop::FfiLibraryFacetCursor cursor;
    cursor.photo_count = source.photo_count;
    cursor.key = source.key.toStdString();
    return cursor;
}

[[nodiscard]] BackendLibraryFacetCursor library_facet_cursor(
    const shadow::desktop::FfiLibraryFacetCursor& source
) {
    return {
        .photo_count = source.photo_count,
        .key = qstring(source.key),
    };
}

[[nodiscard]] BackendLibraryAlbumKind library_album_kind(
    const shadow::desktop::FfiLibraryAlbumKind source
) {
    switch (source) {
    case shadow::desktop::FfiLibraryAlbumKind::Manual:
        return BackendLibraryAlbumKind::Manual;
    case shadow::desktop::FfiLibraryAlbumKind::Smart:
        return BackendLibraryAlbumKind::Smart;
    }
    throw std::invalid_argument("unknown Library album kind");
}

[[nodiscard]] BackendLibraryAlbum library_album(
    const shadow::desktop::FfiLibraryAlbum& source
) {
    return {
        .id = qstring(source.id),
        .kind = library_album_kind(source.kind),
        .name = qstring(source.name),
        .query_filter = library_filter(source.query_filter),
        .created_at_ms = source.created_at_ms,
        .updated_at_ms = source.updated_at_ms,
    };
}

[[nodiscard]] BackendLibrarySourceHealth library_source_health(
    const shadow::desktop::FfiLibrarySourceHealth& source
) {
    return {
        .source_id = qstring(source.source_id),
        .source_display_path = qstring(source.source_display_path),
        .source_enabled = source.source_enabled,
        .has_latest_completed_scan = source.has_latest_completed_scan,
        .scan_session_id = qstring(source.scan_session_id),
        .scan_completed_at_ms = source.scan_completed_at_ms,
        .known_locations = source.known_locations,
        .seen_locations = source.seen_locations,
        .not_seen_locations = source.not_seen_locations,
    };
}

[[nodiscard]] BackendMissingSourceLocation missing_source_location(
    const shadow::desktop::FfiMissingSourceLocation& source
) {
    return {
        .location_id = qstring(source.location_id),
        .photo_id = qstring(source.photo_id),
        .title = qstring(source.title),
        .source_display_path = qstring(source.source_display_path),
        .has_captured_at = source.has_captured_at,
        .captured_at_unix_seconds = source.captured_at_unix_seconds,
        .camera_key = qstring(source.camera_key),
        .last_seen_at_ms = source.last_seen_at_ms,
    };
}

[[nodiscard]] BackendVerifiedSourceRelinkReceipt verified_source_relink_receipt(
    const shadow::desktop::FfiVerifiedSourceRelinkReceipt& source
) {
    return {
        .photo_id = qstring(source.photo_id),
        .representation_id = qstring(source.representation_id),
        .location_id = qstring(source.location_id),
        .display_path = qstring(source.display_path),
    };
}

[[nodiscard]] shadow::desktop::FfiLibraryPhotoCursor ffi_library_cursor(
    const BackendLibraryPhotoCursor& source
) {
    shadow::desktop::FfiLibraryPhotoCursor cursor;
    cursor.photo_id = source.photo_id.toStdString();
    cursor.has_capture_time = source.has_capture_time;
    cursor.captured_at_unix_seconds = source.captured_at_unix_seconds;
    return cursor;
}

[[nodiscard]] BackendLibraryPhotoCursor library_cursor(
    const shadow::desktop::FfiLibraryPhotoCursor& source
) {
    return {
        .photo_id = qstring(source.photo_id),
        .has_capture_time = source.has_capture_time,
        .captured_at_unix_seconds = source.captured_at_unix_seconds,
    };
}

/// Converts the intentionally compact photo-first Catalog projection into the
/// existing grid DTO. Low-frequency technical inspection fields remain empty:
/// the virtualized grid must not make a per-thumbnail technical query.
[[nodiscard]] BackendReviewItem library_review_item(
    const shadow::desktop::FfiLibraryPhotoItem& source
) {
    return {
        .photo_id = qstring(source.photo_id),
        .representation_id = qstring(source.representation_id),
        .visual_handle = qstring(source.visual_handle),
        .decision_head_sequence = source.decision_head_sequence,
        .decision_flag = decision_flag(source.decision_flag),
        .decision_rating = source.decision_rating,
        .liked = source.liked,
        .color_label = qstring(source.color_label),
        .library_state_updated_at_ms = source.library_state_updated_at_ms,
        .has_development_edits = source.has_development_edits,
        .title = qstring(source.title),
        .source_path = qstring(source.source_path),
        .visual_role = qstring(source.visual_role),
        .visual_width = source.visual_width,
        .visual_height = source.visual_height,
        .has_visual = source.has_visual,
        .has_metadata = source.has_metadata,
        .camera_make = qstring(source.camera_make),
        .camera_model = qstring(source.camera_model),
        .lens_make = qstring(source.lens_make),
        .lens_model = qstring(source.lens_model),
        .captured_at_unix_seconds = source.has_captured_at
            ? source.captured_at_unix_seconds
            : 0,
        .iso_speed = source.has_iso_speed ? source.iso_speed : 0.0,
        .aperture_f_number = source.has_aperture
            ? static_cast<double>(source.aperture_milli) / 1000.0
            : 0.0,
        .focal_length_mm = source.has_focal_length
            ? static_cast<double>(source.focal_length_tenth_mm) / 10.0
            : 0.0,
    };
}

} // namespace

BackendLibraryPhotoPage DesktopBackend::libraryPhotoPage(
    const BackendLibraryPhotoFilter& filter,
    const BackendLibraryPhotoCursor& cursor,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->library_photo_page(
        ffi_library_filter(filter),
        ffi_library_cursor(cursor),
        limit
    );
    BackendLibraryPhotoPage page;
    page.has_more = source.has_more;
    page.next_cursor = library_cursor(source.next_cursor);
    page.items.reserve(checked_qt_vector_size(source.items.size(), "library_page_items"));
    for (const auto& item : source.items) {
        page.items.push_back(library_review_item(item));
    }
    return page;
}

std::uint64_t DesktopBackend::libraryPhotoCount(
    const BackendLibraryPhotoFilter& filter
) const {
    return impl_->session->library_photo_count(ffi_library_filter(filter));
}

BackendLibraryFacetPage DesktopBackend::libraryFacetPage(
    const BackendLibraryPhotoFilter& filter,
    const BackendLibraryFacetKind kind,
    const BackendLibraryFacetCursor& cursor,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->library_facet_page(
        ffi_library_filter(filter),
        ffi_library_facet_kind(kind),
        ffi_library_facet_cursor(cursor),
        limit
    );
    BackendLibraryFacetPage page;
    page.has_more = source.has_more;
    page.next_cursor = library_facet_cursor(source.next_cursor);
    page.items.reserve(checked_qt_vector_size(source.items.size(), "library_facet_items"));
    for (const auto& item : source.items) {
        page.items.push_back({
            .key = qstring(item.key),
            .label = qstring(item.label),
            .photo_count = item.photo_count,
        });
    }
    return page;
}

QVector<BackendLibraryAlbum> DesktopBackend::libraryAlbums() const {
    const auto source = impl_->session->library_albums();
    QVector<BackendLibraryAlbum> albums;
    albums.reserve(checked_qt_vector_size(source.size(), "library_albums"));
    for (const auto& album : source) {
        albums.push_back(library_album(album));
    }
    return albums;
}

QVector<BackendLibrarySourceHealth> DesktopBackend::librarySourceHealth() const {
    const auto source = impl_->session->library_source_health();
    QVector<BackendLibrarySourceHealth> health;
    health.reserve(checked_qt_vector_size(source.size(), "library_source_health"));
    for (const auto& record : source) {
        health.push_back(library_source_health(record));
    }
    return health;
}

BackendMissingSourceLocationPage DesktopBackend::missingSourceLocationPage(
    const QString& scan_session_id,
    const QString& after_location_id,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->missing_source_location_page(
        scan_session_id.toStdString(),
        after_location_id.toStdString(),
        limit
    );
    BackendMissingSourceLocationPage page;
    page.has_scan = source.has_scan;
    page.has_more = source.has_more;
    page.next_location_id = qstring(source.next_location_id);
    page.items.reserve(checked_qt_vector_size(source.items.size(), "missing_source_locations"));
    for (const auto& item : source.items) {
        page.items.push_back(missing_source_location(item));
    }
    return page;
}

BackendVerifiedSourceRelinkReceipt DesktopBackend::relinkMissingSourceLocation(
    const QString& scan_session_id,
    const QString& location_id,
    const QString& candidate_path
) const {
    return verified_source_relink_receipt(
        impl_->session->relink_missing_source_location(
            scan_session_id.toStdString(),
            location_id.toStdString(),
            candidate_path.toStdString()
        )
    );
}

BackendLibraryAlbum DesktopBackend::createManualLibraryAlbum(
    const QString& name
) const {
    return library_album(
        impl_->session->create_manual_library_album(name.toStdString())
    );
}

BackendLibraryAlbum DesktopBackend::createSmartLibraryAlbum(
    const QString& name,
    const BackendLibraryPhotoFilter& query_filter
) const {
    return library_album(impl_->session->create_smart_library_album(
        name.toStdString(),
        ffi_library_filter(query_filter)
    ));
}

BackendLibraryAlbum DesktopBackend::renameLibraryAlbum(
    const QString& album_id,
    const QString& name
) const {
    return library_album(impl_->session->rename_library_album(
        album_id.toStdString(),
        name.toStdString()
    ));
}

bool DesktopBackend::deleteLibraryAlbum(const QString& album_id) const {
    return impl_->session->delete_library_album(album_id.toStdString());
}

void DesktopBackend::addPhotoToManualLibraryAlbum(
    const QString& album_id,
    const QString& photo_id
) const {
    impl_->session->add_photo_to_manual_library_album(
        album_id.toStdString(),
        photo_id.toStdString()
    );
}

bool DesktopBackend::removePhotoFromManualLibraryAlbum(
    const QString& album_id,
    const QString& photo_id
) const {
    return impl_->session->remove_photo_from_manual_library_album(
        album_id.toStdString(),
        photo_id.toStdString()
    );
}

BackendPhotoLibraryState DesktopBackend::setPhotoLibraryState(
    const QString& photo_id,
    const bool liked,
    const QString& color_label
) const {
    const auto state = impl_->session->set_photo_library_state(
        photo_id.toStdString(),
        liked,
        color_label.toStdString()
    );
    return {
        .photo_id = qstring(state.photo_id),
        .liked = state.liked,
        .color_label = qstring(state.color_label),
        .updated_at_ms = state.updated_at_ms,
    };
}

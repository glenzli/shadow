#include "backend/desktop_backend_private.hpp"
#include "backend/review_projection.hpp"
#include "backend/rust_qt_projection.hpp"

#include <stdexcept>

namespace {

using desktop_backend_projection::checked_qt_vector_size;
using desktop_backend_projection::decision_flag;
using desktop_backend_projection::qstring;

[[nodiscard]] rust::Vec<rust::String> ffi_strings(const QStringList& source) {
    rust::Vec<rust::String> result;
    result.reserve(static_cast<std::size_t>(source.size()));
    for (const QString& value : source) {
        result.push_back(value.toStdString());
    }
    return result;
}

[[nodiscard]] QStringList string_list(const rust::Vec<rust::String>& source) {
    QStringList result;
    result.reserve(checked_qt_vector_size(source.size(), "library_string_list"));
    for (const auto& value : source) {
        result.push_back(qstring(value));
    }
    return result;
}

[[nodiscard]] rust::Vec<shadow::desktop::FfiLibraryLivingPlaceRule>
ffi_living_place_rules(const QVector<BackendLibraryLivingPlaceRule>& source) {
    rust::Vec<shadow::desktop::FfiLibraryLivingPlaceRule> result;
    result.reserve(static_cast<std::size_t>(source.size()));
    for (const BackendLibraryLivingPlaceRule& rule : source) {
        shadow::desktop::FfiLibraryLivingPlaceRule projected;
        projected.locality_key = rule.locality_key.toStdString();
        projected.start_month = rule.start_month.toStdString();
        projected.end_month = rule.end_month.toStdString();
        result.push_back(std::move(projected));
    }
    return result;
}

[[nodiscard]] QVector<BackendLibraryLivingPlaceRule>
living_place_rules(const rust::Vec<shadow::desktop::FfiLibraryLivingPlaceRule>& source) {
    QVector<BackendLibraryLivingPlaceRule> result;
    result.reserve(checked_qt_vector_size(source.size(), "library_living_place_rules"));
    for (const auto& rule : source) {
        result.push_back({
            .locality_key = qstring(rule.locality_key),
            .start_month = qstring(rule.start_month),
            .end_month = qstring(rule.end_month),
        });
    }
    return result;
}

[[nodiscard]] shadow::desktop::FfiLibraryFlagFilter
ffi_library_flag(const BackendLibraryFlagFilter flag) {
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

[[nodiscard]] shadow::desktop::FfiLibraryPhotoFilter
ffi_library_filter(const BackendLibraryPhotoFilter& source) {
    shadow::desktop::FfiLibraryPhotoFilter filter;
    filter.has_capture_start = source.has_capture_start;
    filter.capture_start_unix_seconds = source.capture_start_unix_seconds;
    filter.has_capture_end = source.has_capture_end;
    filter.capture_end_unix_seconds = source.capture_end_unix_seconds;
    filter.capture_month = source.capture_month.toStdString();
    filter.has_chinese_lunar_month = source.has_chinese_lunar_month;
    filter.chinese_lunar_month = source.chinese_lunar_month;
    filter.has_chinese_lunar_day = source.has_chinese_lunar_day;
    filter.chinese_lunar_day = source.chinese_lunar_day;
    filter.has_chinese_lunar_is_leap_month = source.has_chinese_lunar_is_leap_month;
    filter.chinese_lunar_is_leap_month = source.chinese_lunar_is_leap_month;
    filter.camera_key = source.camera_key.toStdString();
    filter.lens_key = source.lens_key.toStdString();
    filter.country_key = source.country_key.toStdString();
    filter.locality_key = source.locality_key.toStdString();
    filter.living_place_rules = ffi_living_place_rules(source.living_place_rules);
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
    filter.keyword_ids_all = ffi_strings(source.keyword_ids_all);
    filter.excluded_keyword_ids_any = ffi_strings(source.excluded_keyword_ids_any);
    return filter;
}

[[nodiscard]] BackendLibraryFlagFilter
library_flag_filter(const shadow::desktop::FfiLibraryFlagFilter source) {
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

[[nodiscard]] BackendLibraryPhotoFilter
library_filter(const shadow::desktop::FfiLibraryPhotoFilter& source) {
    return {
        .has_capture_start = source.has_capture_start,
        .capture_start_unix_seconds = source.capture_start_unix_seconds,
        .has_capture_end = source.has_capture_end,
        .capture_end_unix_seconds = source.capture_end_unix_seconds,
        .capture_month = qstring(source.capture_month),
        .has_chinese_lunar_month = source.has_chinese_lunar_month,
        .chinese_lunar_month = source.chinese_lunar_month,
        .has_chinese_lunar_day = source.has_chinese_lunar_day,
        .chinese_lunar_day = source.chinese_lunar_day,
        .has_chinese_lunar_is_leap_month = source.has_chinese_lunar_is_leap_month,
        .chinese_lunar_is_leap_month = source.chinese_lunar_is_leap_month,
        .camera_key = qstring(source.camera_key),
        .lens_key = qstring(source.lens_key),
        .country_key = qstring(source.country_key),
        .locality_key = qstring(source.locality_key),
        .living_place_rules = living_place_rules(source.living_place_rules),
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
        .keyword_ids_all = string_list(source.keyword_ids_all),
        .excluded_keyword_ids_any = string_list(source.excluded_keyword_ids_any),
    };
}

[[nodiscard]] shadow::desktop::FfiLibraryFacetKind
ffi_library_facet_kind(const BackendLibraryFacetKind kind) {
    switch (kind) {
    case BackendLibraryFacetKind::CaptureMonth:
        return shadow::desktop::FfiLibraryFacetKind::CaptureMonth;
    case BackendLibraryFacetKind::Camera:
        return shadow::desktop::FfiLibraryFacetKind::Camera;
    case BackendLibraryFacetKind::Lens:
        return shadow::desktop::FfiLibraryFacetKind::Lens;
    case BackendLibraryFacetKind::Country:
        return shadow::desktop::FfiLibraryFacetKind::Country;
    case BackendLibraryFacetKind::City:
        return shadow::desktop::FfiLibraryFacetKind::City;
    }
    throw std::invalid_argument("unknown Library facet kind");
}

[[nodiscard]] shadow::desktop::FfiLibraryPhotoOrder
ffi_library_photo_order(const BackendLibraryPhotoOrder order) {
    switch (order) {
    case BackendLibraryPhotoOrder::CaptureTimeDescending:
        return shadow::desktop::FfiLibraryPhotoOrder::CaptureTimeDescending;
    case BackendLibraryPhotoOrder::CaptureTimeAscending:
        return shadow::desktop::FfiLibraryPhotoOrder::CaptureTimeAscending;
    case BackendLibraryPhotoOrder::FileNameAscending:
        return shadow::desktop::FfiLibraryPhotoOrder::FileNameAscending;
    case BackendLibraryPhotoOrder::FileNameDescending:
        return shadow::desktop::FfiLibraryPhotoOrder::FileNameDescending;
    }
    throw std::invalid_argument("unknown Library photo order");
}

[[nodiscard]] shadow::desktop::FfiLibraryFacetCursor
ffi_library_facet_cursor(const BackendLibraryFacetCursor& source) {
    shadow::desktop::FfiLibraryFacetCursor cursor;
    cursor.photo_count = source.photo_count;
    cursor.key = source.key.toStdString();
    return cursor;
}

[[nodiscard]] BackendLibraryFacetCursor
library_facet_cursor(const shadow::desktop::FfiLibraryFacetCursor& source) {
    return {
        .photo_count = source.photo_count,
        .key = qstring(source.key),
    };
}

[[nodiscard]] BackendLibraryAlbumKind
library_album_kind(const shadow::desktop::FfiLibraryAlbumKind source) {
    switch (source) {
    case shadow::desktop::FfiLibraryAlbumKind::Manual:
        return BackendLibraryAlbumKind::Manual;
    case shadow::desktop::FfiLibraryAlbumKind::Smart:
        return BackendLibraryAlbumKind::Smart;
    }
    throw std::invalid_argument("unknown Library album kind");
}

[[nodiscard]] BackendLibraryAlbum library_album(const shadow::desktop::FfiLibraryAlbum& source) {
    return {
        .id = qstring(source.id),
        .kind = library_album_kind(source.kind),
        .name = qstring(source.name),
        .query_filter = library_filter(source.query_filter),
        .created_at_ms = source.created_at_ms,
        .updated_at_ms = source.updated_at_ms,
    };
}

[[nodiscard]] BackendLibraryKeyword
library_keyword(const shadow::desktop::FfiLibraryKeyword& source) {
    return {
        .id = qstring(source.id),
        .parent_id = qstring(source.parent_id),
        .name = qstring(source.name),
        .depth = source.depth,
        .subtree_photo_count = source.subtree_photo_count,
        .created_at_ms = source.created_at_ms,
        .updated_at_ms = source.updated_at_ms,
    };
}

[[nodiscard]] BackendLibraryKeywordOrigin
library_keyword_origin(const shadow::desktop::FfiLibraryKeywordOrigin source) {
    switch (source) {
    case shadow::desktop::FfiLibraryKeywordOrigin::Manual:
        return BackendLibraryKeywordOrigin::Manual;
    case shadow::desktop::FfiLibraryKeywordOrigin::Imported:
        return BackendLibraryKeywordOrigin::Imported;
    case shadow::desktop::FfiLibraryKeywordOrigin::AiAccepted:
        return BackendLibraryKeywordOrigin::AiAccepted;
    }
    throw std::invalid_argument("unknown Library keyword origin");
}

[[nodiscard]] BackendLibrarySourceHealth
library_source_health(const shadow::desktop::FfiLibrarySourceHealth& source) {
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

[[nodiscard]] BackendMissingSourceLocation
missing_source_location(const shadow::desktop::FfiMissingSourceLocation& source) {
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

[[nodiscard]] BackendVerifiedSourceRelinkReceipt
verified_source_relink_receipt(const shadow::desktop::FfiVerifiedSourceRelinkReceipt& source) {
    return {
        .photo_id = qstring(source.photo_id),
        .representation_id = qstring(source.representation_id),
        .location_id = qstring(source.location_id),
        .display_path = qstring(source.display_path),
        .library_root_path = qstring(source.library_root_path),
    };
}

[[nodiscard]] shadow::desktop::FfiLibraryPhotoCursor
ffi_library_cursor(const BackendLibraryPhotoCursor& source) {
    shadow::desktop::FfiLibraryPhotoCursor cursor;
    cursor.photo_id = source.photo_id.toStdString();
    cursor.has_capture_time = source.has_capture_time;
    cursor.captured_at_unix_seconds = source.captured_at_unix_seconds;
    cursor.file_name = source.file_name.toStdString();
    return cursor;
}

[[nodiscard]] BackendLibraryPhotoCursor
library_cursor(const shadow::desktop::FfiLibraryPhotoCursor& source) {
    return {
        .photo_id = qstring(source.photo_id),
        .has_capture_time = source.has_capture_time,
        .captured_at_unix_seconds = source.captured_at_unix_seconds,
        .file_name = qstring(source.file_name),
    };
}

/// Converts the intentionally compact photo-first Catalog projection into the
/// existing grid DTO. Low-frequency technical inspection fields remain empty:
/// the virtualized grid must not make a per-thumbnail technical query.
[[nodiscard]] BackendReviewItem
library_review_item(const shadow::desktop::FfiLibraryPhotoItem& source) {
    return {
        .photo_id = qstring(source.photo_id),
        .representation_id = qstring(source.representation_id),
        .location_id = qstring(source.location_id),
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
        .source_available = source.source_available,
        .visual_role = qstring(source.visual_role),
        .visual_width = source.visual_width,
        .visual_height = source.visual_height,
        .has_visual = source.has_visual,
        .has_metadata = source.has_metadata,
        .camera_make = qstring(source.camera_make),
        .camera_model = qstring(source.camera_model),
        .lens_make = qstring(source.lens_make),
        .lens_model = qstring(source.lens_model),
        .captured_at_unix_seconds = source.has_captured_at ? source.captured_at_unix_seconds : 0,
        .iso_speed = source.has_iso_speed ? source.iso_speed : 0.0,
        .aperture_f_number =
            source.has_aperture ? static_cast<double>(source.aperture_milli) / 1000.0 : 0.0,
        .focal_length_mm = source.has_focal_length
                               ? static_cast<double>(source.focal_length_tenth_mm) / 10.0
                               : 0.0,
    };
}

[[nodiscard]] BackendLibraryMetadataState
library_metadata_state(const shadow::desktop::FfiLibraryMetadataState& source) {
    return {
        .photo_id = qstring(source.photo_id),
        .has_observed_capture_time = source.has_observed_capture_time,
        .observed_captured_at_unix_seconds = source.observed_captured_at_unix_seconds,
        .has_effective_capture_time = source.has_effective_capture_time,
        .effective_captured_at_unix_seconds = source.effective_captured_at_unix_seconds,
        .capture_time_override_mode = qstring(source.capture_time_override_mode),
        .capture_time_override_origin = qstring(source.capture_time_override_origin),
        .capture_time_source_label = qstring(source.capture_time_source_label),
        .has_observed_coordinates = source.has_observed_coordinates,
        .observed_latitude_e7 = source.observed_latitude_e7,
        .observed_longitude_e7 = source.observed_longitude_e7,
        .has_effective_coordinates = source.has_effective_coordinates,
        .effective_latitude_e7 = source.effective_latitude_e7,
        .effective_longitude_e7 = source.effective_longitude_e7,
        .effective_place_name = qstring(source.effective_place_name),
        .coordinates_override_mode = qstring(source.coordinates_override_mode),
        .coordinates_override_origin = qstring(source.coordinates_override_origin),
        .coordinates_source_label = qstring(source.coordinates_source_label),
    };
}

[[nodiscard]] BackendGpxImportPreview
gpx_import_preview(const shadow::desktop::FfiGpxImportPreview& source) {
    BackendGpxImportPreview result{
        .preview_id = qstring(source.preview_id),
        .source_path = qstring(source.source_path),
        .source_digest_hex = qstring(source.source_digest_hex),
        .requested_photo_count = source.requested_photo_count,
        .matched_photo_count = source.matched_photo_count,
        .unmatched_photo_count = source.unmatched_photo_count,
    };
    result.proposal_sample.reserve(
        checked_qt_vector_size(source.proposal_sample.size(), "gpx_proposal_sample")
    );
    for (const auto& proposal : source.proposal_sample) {
        result.proposal_sample.push_back({
            .photo_id = qstring(proposal.photo_id),
            .captured_at_unix_seconds = proposal.captured_at_unix_seconds,
            .matched_at_unix_seconds = proposal.matched_at_unix_seconds,
            .nearest_track_delta_seconds = proposal.nearest_track_delta_seconds,
            .latitude_e7 = proposal.latitude_e7,
            .longitude_e7 = proposal.longitude_e7,
        });
    }
    return result;
}

[[nodiscard]] BackendCaptureTimeBatchPreview
capture_time_batch_preview(const shadow::desktop::FfiCaptureTimeBatchPreview& source) {
    BackendCaptureTimeBatchPreview result{
        .preview_id = qstring(source.preview_id),
        .mode = qstring(source.mode),
        .offset_seconds = source.offset_seconds,
        .requested_photo_count = source.requested_photo_count,
        .applicable_photo_count = source.applicable_photo_count,
        .skipped_photo_count = source.skipped_photo_count,
    };
    result.proposal_sample.reserve(
        checked_qt_vector_size(source.proposal_sample.size(), "capture_time_proposal_sample")
    );
    for (const auto& proposal : source.proposal_sample) {
        result.proposal_sample.push_back({
            .photo_id = qstring(proposal.photo_id),
            .has_before_capture_time = proposal.has_before_capture_time,
            .before_captured_at_unix_seconds = proposal.before_captured_at_unix_seconds,
            .has_after_capture_time = proposal.has_after_capture_time,
            .after_captured_at_unix_seconds = proposal.after_captured_at_unix_seconds,
        });
    }
    return result;
}

[[nodiscard]] rust::Vec<shadow::desktop::FfiBatchPhotoTarget>
ffi_batch_photo_targets(const QVector<BackendBatchPhotoTarget>& targets) {
    rust::Vec<shadow::desktop::FfiBatchPhotoTarget> result;
    result.reserve(static_cast<std::size_t>(targets.size()));
    for (const auto& target : targets) {
        shadow::desktop::FfiBatchPhotoTarget ffi_target;
        ffi_target.photo_id = target.photo_id.toStdString();
        ffi_target.source_path = target.source_path.toStdString();
        result.push_back(std::move(ffi_target));
    }
    return result;
}

} // namespace

BackendLibraryPhotoPage DesktopBackend::libraryPhotoPage(
    const BackendLibraryPhotoFilter& filter,
    const BackendLibraryPhotoOrder order,
    const BackendLibraryPhotoCursor& cursor,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->library_photo_page(
        ffi_library_filter(filter),
        ffi_library_photo_order(order),
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

std::uint64_t DesktopBackend::libraryPhotoCount(const BackendLibraryPhotoFilter& filter) const {
    return impl_->session->library_photo_count(ffi_library_filter(filter));
}

BackendLibraryMapSnapshot DesktopBackend::libraryMapSnapshot(
    const BackendLibraryPhotoFilter& filter,
    const BackendLibraryMapViewport& viewport,
    const BackendLibraryMapGrid& grid
) const {
    const auto source = impl_->session->library_map_snapshot(
        ffi_library_filter(filter),
        viewport.south_latitude_e7,
        viewport.west_longitude_e7,
        viewport.north_latitude_e7,
        viewport.east_longitude_e7,
        grid.columns,
        grid.rows
    );
    BackendLibraryMapSnapshot snapshot;
    snapshot.photo_count = source.photo_count;
    snapshot.clusters.reserve(
        checked_qt_vector_size(source.clusters.size(), "library_map_clusters")
    );
    for (const auto& cluster : source.clusters) {
        snapshot.clusters.push_back({
            .cell_x = cluster.cell_x,
            .cell_y = cluster.cell_y,
            .latitude_e7 = cluster.latitude_e7,
            .longitude_e7 = cluster.longitude_e7,
            .photo_count = cluster.photo_count,
            .photo_id = qstring(cluster.photo_id),
            .representation_id = qstring(cluster.representation_id),
            .title = qstring(cluster.title),
            .source_path = qstring(cluster.source_path),
        });
    }
    return snapshot;
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

QVector<BackendLibraryPlaceResolutionCandidate>
DesktopBackend::libraryPlaceResolutionCandidates(const std::uint32_t limit) const {
    const auto source = impl_->session->library_place_resolution_candidates(limit);
    QVector<BackendLibraryPlaceResolutionCandidate> candidates;
    candidates.reserve(
        checked_qt_vector_size(source.size(), "library_place_resolution_candidates")
    );
    for (const auto& candidate : source) {
        candidates.push_back({
            .latitude_e7 = candidate.latitude_e7,
            .longitude_e7 = candidate.longitude_e7,
            .photo_count = candidate.photo_count,
        });
    }
    return candidates;
}

BackendRecordLibraryPlaceResolutionStatus DesktopBackend::recordLibraryPlaceResolution(
    const BackendLibraryPlaceResolutionResult& result
) const {
    shadow::desktop::FfiLibraryPlaceResolutionResult ffi_result;
    ffi_result.latitude_e7 = result.latitude_e7;
    ffi_result.longitude_e7 = result.longitude_e7;
    ffi_result.country_code = result.country_code.toStdString();
    ffi_result.country_name = result.country_name.toStdString();
    ffi_result.administrative_area = result.administrative_area.toStdString();
    ffi_result.locality = result.locality.toStdString();
    ffi_result.display_name = result.display_name.toStdString();
    ffi_result.provider_id = result.provider_id.toStdString();
    ffi_result.provider_version = result.provider_version.toStdString();
    ffi_result.locale = result.locale.toStdString();
    switch (impl_->session->record_library_place_resolution(ffi_result)) {
    case shadow::desktop::FfiRecordLibraryPlaceResolutionStatus::Recorded:
        return BackendRecordLibraryPlaceResolutionStatus::Recorded;
    case shadow::desktop::FfiRecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed:
        return BackendRecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed;
    }
    throw std::invalid_argument("unknown Library place resolution write status");
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

QVector<BackendLibraryKeyword> DesktopBackend::libraryKeywords() const {
    const auto source = impl_->session->library_keywords();
    QVector<BackendLibraryKeyword> keywords;
    keywords.reserve(checked_qt_vector_size(source.size(), "library_keywords"));
    for (const auto& value : source) {
        keywords.push_back(library_keyword(value));
    }
    return keywords;
}

QVector<BackendLibraryPhotoKeyword>
DesktopBackend::libraryKeywordsForPhoto(const QString& photo_id) const {
    const auto source = impl_->session->library_keywords_for_photo(photo_id.toStdString());
    QVector<BackendLibraryPhotoKeyword> keywords;
    keywords.reserve(checked_qt_vector_size(source.size(), "library_photo_keywords"));
    for (const auto& value : source) {
        keywords.push_back({
            .keyword = library_keyword(value.keyword),
            .origin = library_keyword_origin(value.origin),
            .source_label = qstring(value.source_label),
            .has_confidence = value.has_confidence,
            .confidence_milli = value.confidence_milli,
            .assigned_at_ms = value.assigned_at_ms,
        });
    }
    return keywords;
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

bool DesktopBackend::removeLibrarySource(const QString& source_id) const {
    return impl_->session->remove_library_source(source_id.toStdString());
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
    return verified_source_relink_receipt(impl_->session->relink_missing_source_location(
        scan_session_id.toStdString(),
        location_id.toStdString(),
        candidate_path.toStdString()
    ));
}

BackendVerifiedSourceRelinkReceipt DesktopBackend::relinkLibrarySourceLocation(
    const QString& location_id,
    const QString& candidate_path
) const {
    return verified_source_relink_receipt(impl_->session->relink_library_source_location(
        location_id.toStdString(),
        candidate_path.toStdString()
    ));
}

BackendLibrarySourceRecoveryReceipt DesktopBackend::recoverLibrarySource(
    const QString& source_id,
    const QString& replacement_folder
) const {
    const auto receipt = impl_->session->recover_library_source(
        source_id.toStdString(),
        replacement_folder.toStdString()
    );
    return {
        .library_root_path = qstring(receipt.library_root_path),
        .recovered_photo_count = receipt.recovered_photo_count,
        .unresolved_photo_count = receipt.unresolved_photo_count,
        .retired_unavailable_source = receipt.retired_unavailable_source,
    };
}

BackendSourceReconciliationReceipt
DesktopBackend::reconcileMissingSourcePhotos(const QString& scan_session_id) const {
    const auto receipt =
        impl_->session->reconcile_missing_source_photos(scan_session_id.toStdString());
    return {
        .reviewed = receipt.reviewed,
        .archived = receipt.archived,
        .retained_available = receipt.retained_available,
    };
}

bool DesktopBackend::archiveLibraryPhoto(const QString& photo_id) const {
    return impl_->session->archive_library_photo(photo_id.toStdString());
}

BackendLibraryAlbum DesktopBackend::createManualLibraryAlbum(const QString& name) const {
    return library_album(impl_->session->create_manual_library_album(name.toStdString()));
}

BackendLibraryKeyword
DesktopBackend::createLibraryKeyword(const QString& parent_id, const QString& name) const {
    return library_keyword(
        impl_->session->create_library_keyword(parent_id.toStdString(), name.toStdString())
    );
}

BackendLibraryKeyword
DesktopBackend::renameLibraryKeyword(const QString& keyword_id, const QString& name) const {
    return library_keyword(
        impl_->session->rename_library_keyword(keyword_id.toStdString(), name.toStdString())
    );
}

BackendLibraryKeyword
DesktopBackend::moveLibraryKeyword(const QString& keyword_id, const QString& parent_id) const {
    return library_keyword(
        impl_->session->move_library_keyword(keyword_id.toStdString(), parent_id.toStdString())
    );
}

BackendLibraryKeywordDeletionReceipt
DesktopBackend::deleteLibraryKeywordSubtree(const QString& keyword_id) const {
    const auto receipt = impl_->session->delete_library_keyword_subtree(keyword_id.toStdString());
    return {
        .deleted_keyword_count = receipt.deleted_keyword_count,
        .deleted_assignment_count = receipt.deleted_assignment_count,
    };
}

BackendLibraryKeywordMutationReceipt DesktopBackend::assignLibraryKeyword(
    const QString& keyword_id,
    const QStringList& photo_ids
) const {
    const auto receipt =
        impl_->session->assign_library_keyword(keyword_id.toStdString(), ffi_strings(photo_ids));
    return {
        .keyword_id = qstring(receipt.keyword_id),
        .requested_photo_count = receipt.requested_photo_count,
        .changed_photo_count = receipt.changed_photo_count,
    };
}

BackendLibraryKeywordMutationReceipt DesktopBackend::removeLibraryKeyword(
    const QString& keyword_id,
    const QStringList& photo_ids
) const {
    const auto receipt =
        impl_->session->remove_library_keyword(keyword_id.toStdString(), ffi_strings(photo_ids));
    return {
        .keyword_id = qstring(receipt.keyword_id),
        .requested_photo_count = receipt.requested_photo_count,
        .changed_photo_count = receipt.changed_photo_count,
    };
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

BackendLibraryAlbum
DesktopBackend::renameLibraryAlbum(const QString& album_id, const QString& name) const {
    return library_album(
        impl_->session->rename_library_album(album_id.toStdString(), name.toStdString())
    );
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

BackendLibraryMetadataState DesktopBackend::libraryMetadataState(const QString& photo_id) const {
    return library_metadata_state(impl_->session->library_metadata_state(photo_id.toStdString()));
}

BackendLibraryMetadataState DesktopBackend::setLibraryCaptureTimeOverride(
    const QString& photo_id,
    const QString& mode,
    const std::int64_t captured_at_unix_seconds
) const {
    return library_metadata_state(impl_->session->set_library_capture_time_override(
        photo_id.toStdString(),
        mode.toStdString(),
        captured_at_unix_seconds
    ));
}

BackendLibraryMetadataState DesktopBackend::setLibraryCoordinatesOverride(
    const QString& photo_id,
    const QString& mode,
    const double latitude_degrees,
    const double longitude_degrees,
    const QString& place_name
) const {
    return library_metadata_state(impl_->session->set_library_coordinates_override(
        photo_id.toStdString(),
        mode.toStdString(),
        latitude_degrees,
        longitude_degrees,
        place_name.toStdString()
    ));
}

BackendCaptureTimeBatchPreview DesktopBackend::previewLibraryCaptureTimeBatch(
    const QVector<BackendBatchPhotoTarget>& targets,
    const QString& mode,
    const std::int64_t offset_seconds
) const {
    return capture_time_batch_preview(impl_->session->preview_library_capture_time_batch(
        ffi_batch_photo_targets(targets),
        mode.toStdString(),
        offset_seconds
    ));
}

BackendLibraryMetadataBatchReceipt
DesktopBackend::applyLibraryCaptureTimeBatch(const QString& preview_id) const {
    const auto receipt = impl_->session->apply_library_capture_time_batch(preview_id.toStdString());
    return {
        .requested_photo_count = receipt.requested_photo_count,
        .applied_photo_count = receipt.applied_photo_count,
    };
}

BackendGpxImportPreview DesktopBackend::previewLibraryGpxImport(
    const QString& gpx_path,
    const QVector<BackendBatchPhotoTarget>& targets,
    const std::int64_t camera_clock_offset_seconds,
    const std::uint32_t maximum_gap_seconds
) const {
    return gpx_import_preview(impl_->session->preview_library_gpx_import(
        gpx_path.toStdString(),
        ffi_batch_photo_targets(targets),
        camera_clock_offset_seconds,
        maximum_gap_seconds
    ));
}

BackendLibraryMetadataBatchReceipt
DesktopBackend::applyLibraryGpxImport(const QString& preview_id) const {
    const auto receipt = impl_->session->apply_library_gpx_import(preview_id.toStdString());
    return {
        .requested_photo_count = receipt.requested_photo_count,
        .applied_photo_count = receipt.applied_photo_count,
    };
}

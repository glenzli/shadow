#include "desktop_backend.hpp"

#include "backend/edit_settings_projection.hpp"
#include "backend/export_backend.hpp"
#include "backend/rust_qt_projection.hpp"
#include "photo_inspection_projection.hpp"
#include "preview_diagnostics.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QImage>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using desktop_backend_projection::checked_qt_vector_size;
using desktop_backend_projection::edit_state;
using desktop_backend_projection::ffi_edit_preview_policy;
using desktop_backend_projection::ffi_grade_node;
using desktop_backend_projection::ffi_grade_stack;
using desktop_backend_projection::grade_node;
using desktop_backend_projection::qbytes;
using desktop_backend_projection::qcounts;
using desktop_backend_projection::qstring;
using desktop_backend_projection::shared_grade_node;

[[nodiscard]] shadow::desktop::FfiPairwiseOutcome ffi_outcome(
    const BackendPairwiseOutcome outcome
) {
    switch (outcome) {
    case BackendPairwiseOutcome::LeftPreferred:
        return shadow::desktop::FfiPairwiseOutcome::LeftPreferred;
    case BackendPairwiseOutcome::RightPreferred:
        return shadow::desktop::FfiPairwiseOutcome::RightPreferred;
    case BackendPairwiseOutcome::KeepBoth:
        return shadow::desktop::FfiPairwiseOutcome::KeepBoth;
    case BackendPairwiseOutcome::KeepNeither:
        return shadow::desktop::FfiPairwiseOutcome::KeepNeither;
    case BackendPairwiseOutcome::CannotCompare:
        return shadow::desktop::FfiPairwiseOutcome::CannotCompare;
    }
    throw std::invalid_argument("unknown pairwise outcome");
}

[[nodiscard]] BackendReviewDecisionFlag decision_flag(
    const shadow::desktop::FfiDecisionFlag flag
) {
    switch (flag) {
    case shadow::desktop::FfiDecisionFlag::Unflagged:
        return BackendReviewDecisionFlag::Unflagged;
    case shadow::desktop::FfiDecisionFlag::Picked:
        return BackendReviewDecisionFlag::Picked;
    case shadow::desktop::FfiDecisionFlag::Rejected:
        return BackendReviewDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

[[nodiscard]] shadow::desktop::FfiDecisionFlag ffi_decision_flag(
    const BackendReviewDecisionFlag flag
) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return shadow::desktop::FfiDecisionFlag::Unflagged;
    case BackendReviewDecisionFlag::Picked:
        return shadow::desktop::FfiDecisionFlag::Picked;
    case BackendReviewDecisionFlag::Rejected:
        return shadow::desktop::FfiDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

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

struct DesktopBackend::Impl final {
    explicit Impl(rust::Box<shadow::desktop::DesktopSession> value)
        : session(std::move(value)),
          folder_scan_backend(*session),
          export_backend(*session) {}

    rust::Box<shadow::desktop::DesktopSession> session;
    FolderScanBackend folder_scan_backend;
    ExportBackend export_backend;
};

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

BackendReviewVisual DesktopBackend::loadReviewVisual(const QString& ticket) const {
    const auto payload = impl_->session->load_review_visual(ticket.toStdString());
    return {
        .bytes = qbytes(payload.bytes),
        .requires_frame_receipt = payload.requires_frame_receipt,
    };
}

BackendReviewComparisonPresentation DesktopBackend::prepareReviewComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) const {
    const auto presentation = impl_->session->prepare_review_comparison(
        left_visual_handle.toStdString(),
        right_visual_handle.toStdString()
    );
    return {
        .presentation_id = qstring(presentation.presentation_id),
        .left_request_ticket = qstring(presentation.left_request_ticket),
        .right_request_ticket = qstring(presentation.right_request_ticket),
    };
}

void DesktopBackend::reportReviewVisualFrame(
    const QString& ticket,
    const QString& decoder_version,
    const std::uint32_t requested_width,
    const std::uint32_t requested_height,
    const std::uint32_t decoded_width,
    const std::uint32_t decoded_height,
    const QString& pixel_hash_hex
) const {
    impl_->session->record_review_visual_frame(
        ticket.toStdString(),
        decoder_version.toStdString(),
        requested_width,
        requested_height,
        decoded_width,
        decoded_height,
        pixel_hash_hex.toStdString()
    );
}

void DesktopBackend::confirmReviewComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) const {
    impl_->session->confirm_review_comparison_ready(
        presentation_id.toStdString(),
        left_request_ticket.toStdString(),
        right_request_ticket.toStdString()
    );
}

void DesktopBackend::cancelReviewComparison(const QString& presentation_id) const {
    impl_->session->cancel_review_comparison(presentation_id.toStdString());
}

BackendFeedbackReceipt DesktopBackend::recordReviewComparison(
    const QString& presentation_id,
    const BackendPairwiseOutcome outcome
) const {
    const auto receipt = impl_->session->record_review_comparison(
        presentation_id.toStdString(),
        ffi_outcome(outcome)
    );
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendForgetReceipt DesktopBackend::forgetReviewFeedback(
    const QString& event_id
) const {
    const auto receipt = impl_->session->forget_review_feedback(event_id.toStdString());
    return {
        .fact_id = qstring(receipt.fact_id),
        .target_event_id = qstring(receipt.target_event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendReviewDecisionState DesktopBackend::reviewPhotoDecisionState(
    const QString& photo_id
) const {
    const auto state = impl_->session->review_photo_decision_state(photo_id.toStdString());
    return {
        .photo_id = qstring(state.photo_id),
        .head_sequence = state.head_sequence,
        .flag = decision_flag(state.flag),
        .rating = state.rating,
    };
}

BackendReviewDecisionMutationReceipt DesktopBackend::setReviewPhotoDecision(
    const QString& photo_id,
    const std::uint64_t expected_head_sequence,
    const BackendReviewDecisionFlag desired_flag,
    const std::uint8_t desired_rating
) const {
    const auto receipt = impl_->session->set_review_photo_decision(
        photo_id.toStdString(),
        expected_head_sequence,
        ffi_decision_flag(desired_flag),
        desired_rating
    );
    const QString returned_photo_id = qstring(receipt.photo_id);
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
        .before = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.before_head_sequence,
            .flag = decision_flag(receipt.before_flag),
            .rating = receipt.before_rating,
        },
        .after = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.sequence,
            .flag = decision_flag(receipt.after_flag),
            .rating = receipt.after_rating,
        },
    };
}

BackendPhotoEditState DesktopBackend::photoEditState(
    const QString& photo_id,
    const QString& source_path
) const {
    return edit_state(impl_->session->photo_edit_state(
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::resetIncompatiblePhotoEditHistory(
    const QString& photo_id,
    const QString& source_path
) const {
    return edit_state(impl_->session->reset_incompatible_photo_edit_history(
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

QVariantList DesktopBackend::opticsProfileCandidates(
    const QString& photo_id,
    const QString& source_path
) const {
    const auto candidates = impl_->session->optics_profile_candidates(
        photo_id.toStdString(), source_path.toStdString()
    );
    QVariantList result;
    result.reserve(checked_qt_vector_size(candidates.size(), "optics_profile_candidates"));
    for (const auto& candidate : candidates) {
        result.push_back(QVariantMap{
            {QStringLiteral("cameraMaker"), qstring(candidate.camera_maker)},
            {QStringLiteral("cameraModel"), qstring(candidate.camera_model)},
            {QStringLiteral("lensMaker"), qstring(candidate.lens_maker)},
            {QStringLiteral("lensModel"), qstring(candidate.lens_model)},
        });
    }
    return result;
}

QVector<BackendSharedGradeNode> DesktopBackend::sharedGradeNodes() const {
    const auto shared = impl_->session->shared_grade_nodes();
    QVector<BackendSharedGradeNode> result;
    result.reserve(checked_qt_vector_size(shared.size(), "shared_grade_nodes"));
    for (const auto& node : shared) {
        result.push_back(shared_grade_node(node));
    }
    return result;
}

BackendSharedGradeNode DesktopBackend::publishSharedGradeNode(
    const QString& label,
    const BackendGradeNode& grade_node
) const {
    const auto ffi_node = ffi_grade_node(grade_node);
    return shared_grade_node(impl_->session->publish_shared_grade_node(
        label.toStdString(), ffi_node
    ));
}

BackendBatchGradeReceipt DesktopBackend::applySharedGradeNodeToPhotos(
    const QString& layer_id,
    const QVector<BackendBatchPhotoTarget>& targets
) const {
    rust::Vec<shadow::desktop::FfiBatchPhotoTarget> ffi_targets;
    ffi_targets.reserve(static_cast<std::size_t>(targets.size()));
    for (const auto& target : targets) {
        shadow::desktop::FfiBatchPhotoTarget ffi_target;
        ffi_target.photo_id = target.photo_id.toStdString();
        ffi_target.source_path = target.source_path.toStdString();
        ffi_targets.push_back(std::move(ffi_target));
    }
    const auto receipt = impl_->session->apply_shared_grade_node_to_photos(
        layer_id.toStdString(), std::move(ffi_targets)
    );
    BackendBatchGradeReceipt result{
        .requested = receipt.requested,
        .updated = receipt.updated,
        .unchanged = receipt.unchanged,
        .failed = receipt.failed,
    };
    result.errors.reserve(
        checked_qt_vector_size(receipt.errors.size(), "batch_grade_errors")
    );
    for (const auto& error : receipt.errors) {
        result.errors.push_back(qstring(error));
    }
    return result;
}

BackendGradeNode DesktopBackend::newBasicGradeNode(const QString& label) const {
    return grade_node(shadow::desktop::new_basic_grade_node(label.toStdString()));
}

ExportBackend& DesktopBackend::exportBackend() noexcept {
    return impl_->export_backend;
}

BackendCacheMaintenanceInventory DesktopBackend::cacheMaintenanceInventory() const {
    const auto inventory = impl_->session->cache_maintenance_inventory();
    return {
        .catalog_live_blob_count = inventory.catalog_live_blob_count,
        .cache_blob_count = inventory.cache_blob_count,
        .cache_blob_byte_length = inventory.cache_blob_byte_len,
        .unknown_entry_count = inventory.unknown_entry_count,
        .unsupported_algorithm_count = inventory.unsupported_algorithm_count,
    };
}

BackendCacheMaintenanceSweep DesktopBackend::planCacheMaintenanceSweep() const {
    const auto sweep = impl_->session->cache_maintenance_sweep(true);
    return {
        .dry_run = sweep.dry_run,
        .catalog_live_blob_count = sweep.catalog_live_blob_count,
        .cache_blob_count = sweep.cache_blob_count,
        .cache_blob_byte_length = sweep.cache_blob_byte_len,
        .unknown_entry_count = sweep.unknown_entry_count,
        .unsupported_algorithm_count = sweep.unsupported_algorithm_count,
        .retained_blob_count = sweep.retained_blob_count,
        .recently_protected_blob_count = sweep.recently_protected_blob_count,
        .recently_protected_byte_length = sweep.recently_protected_byte_len,
        .reclaimed_blob_count = sweep.reclaimed_blob_count,
        .reclaimed_byte_length = sweep.reclaimed_byte_len,
    };
}

BackendCacheMaintenanceSweep DesktopBackend::runCacheMaintenanceSweep() const {
    const auto sweep = impl_->session->cache_maintenance_sweep(false);
    return {
        .dry_run = sweep.dry_run,
        .catalog_live_blob_count = sweep.catalog_live_blob_count,
        .cache_blob_count = sweep.cache_blob_count,
        .cache_blob_byte_length = sweep.cache_blob_byte_len,
        .unknown_entry_count = sweep.unknown_entry_count,
        .unsupported_algorithm_count = sweep.unsupported_algorithm_count,
        .retained_blob_count = sweep.retained_blob_count,
        .recently_protected_blob_count = sweep.recently_protected_blob_count,
        .recently_protected_byte_length = sweep.recently_protected_byte_len,
        .reclaimed_blob_count = sweep.reclaimed_blob_count,
        .reclaimed_byte_length = sweep.reclaimed_byte_len,
    };
}

BackendEditedPreview DesktopBackend::renderEditPreview(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint64_t render_token,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const EditPreviewPolicy policy
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    std::string ffi_photo_id;
    std::string ffi_source_path;
    try {
        request.base_commit_id = base_commit_id.toStdString();
        request.settings = ffi_grade_stack(grade_stack);
        request.render_token = render_token;
        request.max_edge = max_edge;
        request.jpeg_quality = jpeg_quality;
        request.policy = ffi_edit_preview_policy(policy);
        request.use_working_recipe =
            edit_preview_kind(policy) == EditPreviewKind::Current;
        ffi_photo_id = photo_id.toStdString();
        ffi_source_path = source_path.toStdString();
    } catch (...) {
        const std::exception_ptr construction_error = std::current_exception();
        try {
            if (impl_->session->claim_basic_edit_preview_terminal(render_token)
                == shadow::desktop::FfiEditPreviewTerminal::Cancelled) {
                return {
                    .terminal = EditPreviewTerminal::Cancelled,
                };
            }
        } catch (...) {
            // Preserve the actual request-construction failure. Registry
            // diagnostics cannot make a malformed request more actionable.
        }
        std::rethrow_exception(construction_error);
    }
    const auto payload = impl_->session->render_basic_edit_preview(
        ffi_photo_id,
        ffi_source_path,
        request
    );
    if (payload.terminal == shadow::desktop::FfiEditPreviewTerminal::Cancelled) {
        return {
            .terminal = EditPreviewTerminal::Cancelled,
        };
    }
    if (payload.terminal != shadow::desktop::FfiEditPreviewTerminal::Completed) {
        throw std::runtime_error("edit preview returned an unknown terminal state");
    }
    const QByteArray preview_bytes = qbytes(payload.bytes);
    const QSize preview_dimensions(
        static_cast<int>(payload.width),
        static_cast<int>(payload.height)
    );
    const PreviewSensorClippingMask sensor_clipping{
        .available = payload.sensor_clipping_available,
        .dimensions = QSize(
            static_cast<int>(payload.sensor_clipping_width),
            static_cast<int>(payload.sensor_clipping_height)
        ),
        .samples = qbytes(payload.sensor_clipping_mask),
        .highlight_pixel_count = payload.sensor_highlight_clipped_pixels,
        .shadow_pixel_count = payload.sensor_shadow_clipped_pixels,
    };
    if (payload.analysis_available != edit_preview_requires_analysis(policy)) {
        throw std::runtime_error(
            "edit preview returned analysis inconsistent with its explicit policy"
        );
    }
    return {
        .bytes = preview_bytes,
        .analysis = {
            .available = payload.analysis_available,
            .version = qstring(payload.analysis_version),
            .red = qcounts(payload.red_histogram, "red_histogram"),
            .green = qcounts(payload.green_histogram, "green_histogram"),
            .blue = qcounts(payload.blue_histogram, "blue_histogram"),
            .luma = qcounts(payload.luma_histogram, "luma_histogram"),
            .below_zero_samples = qcounts(
                payload.below_zero_samples,
                "below_zero_samples"
            ),
            .above_one_samples = qcounts(
                payload.above_one_samples,
                "above_one_samples"
            ),
            .hdr_headroom_bins = qcounts(
                payload.hdr_headroom_bins,
                "hdr_headroom_bins"
            ),
            .hdr_headroom_pixels = payload.hdr_headroom_pixels,
            .hdr_peak_headroom_ev = payload.hdr_peak_headroom_ev,
            .width = payload.analysis_width,
            .height = payload.analysis_height,
            .pixel_count = payload.pixel_count,
            .shadow_clipped_pixels = payload.shadow_clipped_pixels,
            .highlight_clipped_pixels = payload.highlight_clipped_pixels,
        },
        // Interactive frames intentionally avoid decoding their just-encoded
        // JPEG a second time merely to build a transient zebra raster.
        .display_zebra = edit_preview_requires_display_diagnostics(policy)
            ? make_clipping_zebra_overlay(
                  preview_dimensions,
                  preview_bytes,
                  sensor_clipping
              )
            : QImage{},
        .optics = {
            .status = qstring(payload.optics_status),
            .provider_id = qstring(payload.optics_provider_id),
            .provider_version = qstring(payload.optics_provider_version),
            .camera_profile = qstring(payload.optics_camera_profile),
            .lens_profile = qstring(payload.optics_lens_profile),
            .distortion_available = payload.optics_distortion_available,
            .tca_available = payload.optics_tca_available,
            .vignetting_available = payload.optics_vignetting_available,
            .applied_distortion = payload.optics_applied_distortion,
            .applied_tca = payload.optics_applied_tca,
            .applied_vignetting = payload.optics_applied_vignetting,
            .vignetting_used_distance_fallback = payload.optics_vignetting_used_distance_fallback,
            .applied_scaling = payload.optics_applied_scaling,
        },
        .width = payload.width,
        .height = payload.height,
        .terminal = EditPreviewTerminal::Completed,
    };
}

std::uint64_t DesktopBackend::beginEditPreviewRequest() const noexcept {
    return impl_->session->begin_basic_edit_preview();
}

bool DesktopBackend::cancelEditPreviewRequest(
    const std::uint64_t render_token
) const noexcept {
    return impl_->session->cancel_basic_edit_preview(render_token);
}

std::uint64_t DesktopBackend::beginEditDetailRequest() const noexcept {
    return impl_->session->begin_basic_edit_detail();
}

BackendEditedDetailViewport DesktopBackend::renderEditDetailViewport(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint64_t render_token,
    const double center_x,
    const double center_y,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height,
    const std::uint32_t tile_side,
    const bool use_working_recipe
) const {
    shadow::desktop::FfiEditDetailViewportRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_grade_stack(grade_stack);
    request.render_token = render_token;
    request.center_x = center_x;
    request.center_y = center_y;
    request.viewport_width = viewport_width;
    request.viewport_height = viewport_height;
    request.tile_side = tile_side;
    request.use_working_recipe = use_working_recipe;
    const auto payload = impl_->session->render_basic_edit_detail_viewport(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
    );
    BackendEditedDetailViewport result;
    result.full_width = payload.full_width;
    result.full_height = payload.full_height;
    result.retained_bytes = payload.retained_bytes;
    result.tiles.reserve(static_cast<qsizetype>(payload.tiles.size()));
    for (const auto& tile : payload.tiles) {
        result.tiles.push_back({
            .bytes = qbytes(tile.bytes),
            .x = tile.x,
            .y = tile.y,
            .width = tile.width,
            .height = tile.height,
            .row_stride_bytes = tile.row_stride_bytes,
        });
    }
    return result;
}

BackendPhotoEditState DesktopBackend::saveEditVersion(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack& grade_stack,
    const QString& version_name
) const {
    const auto ffi = ffi_grade_stack(grade_stack);
    return edit_state(impl_->session->save_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        ffi,
        version_name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::autosaveWorkingEdit(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack& grade_stack
) const {
    const auto ffi = ffi_grade_stack(grade_stack);
    return edit_state(impl_->session->autosave_basic_edit_working(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        ffi
    ));
}

BackendPhotoEditState DesktopBackend::loadEditVersionDraft(
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id
) const {
    return edit_state(impl_->session->checkout_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        commit_id.toStdString()
    ));
}

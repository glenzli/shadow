#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] QByteArray qbytes(const rust::Vec<std::uint8_t>& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QByteArray(
        reinterpret_cast<const char*>(value.data()),
        static_cast<qsizetype>(length)
    );
}

[[nodiscard]] shadow::desktop::FfiBasicEditParameters ffi_parameters(
    const BackendBasicEditParameters& source
) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .red_channel_gain = source.red_channel_gain,
        .green_channel_gain = source.green_channel_gain,
        .blue_channel_gain = source.blue_channel_gain,
        .saturation_factor = source.saturation_factor,
    };
}

[[nodiscard]] BackendBasicEditParameters edit_parameters(
    const shadow::desktop::FfiBasicEditParameters& source
) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .red_channel_gain = source.red_channel_gain,
        .green_channel_gain = source.green_channel_gain,
        .blue_channel_gain = source.blue_channel_gain,
        .saturation_factor = source.saturation_factor,
    };
}

[[nodiscard]] BackendPhotoEditState edit_state(
    const shadow::desktop::FfiPhotoEditState& source
) {
    BackendPhotoEditState state;
    state.photo_id = qstring(source.photo_id);
    state.source_path = qstring(source.source_path);
    state.working_commit_id = qstring(source.working_commit_id);
    state.recipe_id = qstring(source.recipe_id);
    state.parameters = edit_parameters(source.parameters);
    state.has_working_version = source.has_working_version;
    state.versions.reserve(static_cast<qsizetype>(source.versions.size()));
    for (const auto& version : source.versions) {
        BackendEditVersion converted;
        converted.commit_id = qstring(version.commit_id);
        converted.name = qstring(version.name);
        converted.created_at_ms = version.created_at_ms;
        converted.is_working = version.is_working;
        converted.parent_commit_ids.reserve(
            static_cast<qsizetype>(version.parent_commit_ids.size())
        );
        for (const auto& parent : version.parent_commit_ids) {
            converted.parent_commit_ids.push_back(qstring(parent));
        }
        state.versions.push_back(std::move(converted));
    }
    return state;
}

} // namespace

struct DesktopBackend::Impl final {
    explicit Impl(rust::Box<shadow::desktop::DesktopSession> value)
        : session(std::move(value)) {}

    rust::Box<shadow::desktop::DesktopSession> session;
};

DesktopBackend::DesktopBackend(const QString& catalog_path, const QString& cache_root)
    : impl_(std::make_unique<Impl>(shadow::desktop::open_desktop_session(
          catalog_path.toStdString(),
          cache_root.toStdString()
      ))) {}

DesktopBackend::~DesktopBackend() = default;

BackendScanReport DesktopBackend::scanFolder(const QString& folder_path) const {
    const auto source = impl_->session->scan_folder(folder_path.toStdString());
    return {
        .folder_path = qstring(source.folder_path),
        .files_seen = source.files_seen,
        .supported_files = source.supported_files,
        .decode_queued = source.decode_inspections_queued,
        .issue_count = source.issue_count,
    };
}

BackendReviewPage DesktopBackend::reviewPage(
    const QString& cursor_path,
    const QString& cursor_representation_id,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->review_page(
        cursor_path.toStdString(),
        cursor_representation_id.toStdString(),
        limit
    );
    BackendReviewPage page;
    page.total_items = source.total_items;
    page.has_more = source.has_more;
    page.next_cursor_path = qstring(source.next_cursor_path);
    page.next_cursor_representation_id = qstring(source.next_cursor_representation_id);
    page.items.reserve(static_cast<qsizetype>(source.items.size()));
    for (const auto& item : source.items) {
        page.items.push_back({
            .photo_id = qstring(item.photo_id),
            .representation_id = qstring(item.representation_id),
            .title = qstring(item.title),
            .source_path = qstring(item.source_path),
            .visual_role = qstring(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
        });
    }
    return page;
}

QByteArray DesktopBackend::loadReviewVisual(const QString& representation_id) const {
    const auto payload = impl_->session->load_review_visual(representation_id.toStdString());
    return qbytes(payload.bytes);
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

BackendEditedPreview DesktopBackend::renderBasicEditPreview(
    const QString& photo_id,
    const QString& source_path,
    const BackendBasicEditParameters& parameters,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) const {
    const auto ffi = ffi_parameters(parameters);
    const auto payload = impl_->session->render_basic_edit_preview(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi,
        max_edge,
        jpeg_quality
    );
    return {
        .bytes = qbytes(payload.bytes),
        .width = payload.width,
        .height = payload.height,
    };
}

BackendPhotoEditState DesktopBackend::saveBasicEditVersion(
    const QString& photo_id,
    const QString& source_path,
    const BackendBasicEditParameters& parameters,
    const QString& version_name
) const {
    const auto ffi = ffi_parameters(parameters);
    return edit_state(impl_->session->save_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi,
        version_name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::checkoutBasicEditVersion(
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

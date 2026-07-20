#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
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

[[nodiscard]] qsizetype checked_qt_vector_size(
    const std::size_t size,
    const char* const field
) {
    if (size > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error(std::string("desktop bridge vector is too large: ") + field);
    }
    return static_cast<qsizetype>(size);
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
    state.versions.reserve(checked_qt_vector_size(source.versions.size(), "versions"));
    for (const auto& version : source.versions) {
        BackendEditVersion converted;
        converted.commit_id = qstring(version.commit_id);
        converted.name = qstring(version.name);
        converted.created_at_ms = version.created_at_ms;
        converted.is_working = version.is_working;
        converted.is_root = version.is_root;
        converted.recipe_schema_changed = version.recipe_schema_changed;
        converted.layers_added = version.layers_added;
        converted.layers_removed = version.layers_removed;
        converted.layers_moved = version.layers_moved;
        converted.layers_modified = version.layers_modified;
        converted.nodes_added = version.nodes_added;
        converted.nodes_removed = version.nodes_removed;
        converted.nodes_modified = version.nodes_modified;
        converted.node_parameter_blocks_changed = version.node_parameter_blocks_changed;
        converted.changed_basic_parameter_count = version.changed_basic_parameter_count;
        converted.has_other_changes = version.has_other_changes;
        converted.parent_commit_ids.reserve(
            checked_qt_vector_size(version.parent_commit_ids.size(), "parent_commit_ids")
        );
        for (const auto& parent : version.parent_commit_ids) {
            converted.parent_commit_ids.push_back(qstring(parent));
        }
        const auto changed_parameter_size = checked_qt_vector_size(
            version.changed_basic_parameters.size(),
            "changed_basic_parameters"
        );
        if (static_cast<std::uint64_t>(changed_parameter_size)
            != static_cast<std::uint64_t>(version.changed_basic_parameter_count)) {
            throw std::runtime_error(
                "desktop bridge returned an inconsistent changed-basic-parameter count"
            );
        }
        converted.changed_basic_parameters.reserve(changed_parameter_size);
        for (const auto& parameter : version.changed_basic_parameters) {
            converted.changed_basic_parameters.push_back(qstring(parameter));
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
    const QString& base_commit_id,
    const BackendBasicEditParameters& parameters,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const bool use_working_recipe
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.parameters = ffi_parameters(parameters);
    request.max_edge = max_edge;
    request.jpeg_quality = jpeg_quality;
    request.use_working_recipe = use_working_recipe;
    const auto payload = impl_->session->render_basic_edit_preview(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
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
    const QString& base_commit_id,
    const BackendBasicEditParameters& parameters,
    const QString& version_name
) const {
    const auto ffi = ffi_parameters(parameters);
    return edit_state(impl_->session->save_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
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

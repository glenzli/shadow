#include "history_projection.hpp"

#include "rust_qt_projection.hpp"

namespace desktop_backend_projection {
namespace {

[[nodiscard]] BackendHistoryRefKind
history_ref_kind(const shadow::desktop::FfiHistoryRefKind source) {
    switch (source) {
    case shadow::desktop::FfiHistoryRefKind::Working:
        return BackendHistoryRefKind::Working;
    case shadow::desktop::FfiHistoryRefKind::Branch:
        return BackendHistoryRefKind::Branch;
    case shadow::desktop::FfiHistoryRefKind::NamedVersion:
        return BackendHistoryRefKind::NamedVersion;
    case shadow::desktop::FfiHistoryRefKind::Tag:
        return BackendHistoryRefKind::Tag;
    }
    throw std::invalid_argument("unknown desktop History ref kind");
}

[[nodiscard]] BackendHistoryRef history_ref(const shadow::desktop::FfiHistoryRef& source) {
    return {
        .name = qstring(source.name),
        .kind = history_ref_kind(source.kind),
        .commit_id = qstring(source.commit_id),
        .updated_at_ms = source.updated_at_ms,
    };
}

[[nodiscard]] QStringList strings(const rust::Vec<rust::String>& source, const char* const field) {
    QStringList result;
    result.reserve(checked_qt_vector_size(source.size(), field));
    for (const auto& value : source) {
        result.push_back(qstring(value));
    }
    return result;
}

[[nodiscard]] QVector<BackendHistoryRef>
history_refs(const rust::Vec<shadow::desktop::FfiHistoryRef>& source, const char* const field) {
    QVector<BackendHistoryRef> result;
    result.reserve(checked_qt_vector_size(source.size(), field));
    for (const auto& reference : source) {
        result.push_back(history_ref(reference));
    }
    return result;
}

[[nodiscard]] BackendHistoryCursor history_cursor(const shadow::desktop::FfiHistoryCursor& source) {
    return {
        .created_at_ms = source.created_at_ms,
        .commit_id = qstring(source.commit_id),
    };
}

} // namespace

shadow::desktop::FfiHistoryCursor ffi_history_cursor(const BackendHistoryCursor& source) {
    shadow::desktop::FfiHistoryCursor result;
    result.created_at_ms = source.created_at_ms;
    result.commit_id = source.commit_id.toStdString();
    return result;
}

BackendPhotoHistoryPage photo_history_page(const shadow::desktop::FfiPhotoHistoryPage& source) {
    BackendPhotoHistoryPage result{
        .has_more = source.has_more,
        .next_cursor = history_cursor(source.next_cursor),
    };
    result.entries.reserve(checked_qt_vector_size(source.entries.size(), "photo_history_entries"));
    for (const auto& entry : source.entries) {
        result.entries.push_back({
            .commit_id = qstring(entry.commit_id),
            .name = qstring(entry.name),
            .created_at_ms = entry.created_at_ms,
            .parent_commit_ids =
                strings(entry.parent_commit_ids, "photo_history_parent_commit_ids"),
            .refs = history_refs(entry.refs, "photo_history_refs"),
            .is_named = entry.is_named,
            .is_working = entry.is_working,
            .is_root = entry.is_root,
            .recipe_schema_changed = entry.recipe_schema_changed,
            .grade_nodes_added = entry.grade_nodes_added,
            .grade_nodes_removed = entry.grade_nodes_removed,
            .grade_nodes_moved = entry.grade_nodes_moved,
            .grade_nodes_modified = entry.grade_nodes_modified,
            .render_ops_added = entry.render_ops_added,
            .render_ops_removed = entry.render_ops_removed,
            .render_ops_modified = entry.render_ops_modified,
            .render_op_parameter_blocks_changed = entry.render_op_parameter_blocks_changed,
            .changed_parameter_keys =
                strings(entry.changed_parameter_keys, "photo_history_changed_parameter_keys"),
            .has_other_changes = entry.has_other_changes,
        });
    }
    return result;
}

BackendLibraryHistoryPage
library_history_page(const shadow::desktop::FfiLibraryHistoryPage& source) {
    BackendLibraryHistoryPage result{
        .has_more = source.has_more,
        .next_cursor = history_cursor(source.next_cursor),
    };
    result.entries.reserve(
        checked_qt_vector_size(source.entries.size(), "library_history_entries")
    );
    for (const auto& entry : source.entries) {
        result.entries.push_back({
            .commit_id = qstring(entry.commit_id),
            .message = qstring(entry.message),
            .created_at_ms = entry.created_at_ms,
            .parent_commit_ids =
                strings(entry.parent_commit_ids, "library_history_parent_commit_ids"),
            .refs = history_refs(entry.refs, "library_history_refs"),
            .is_root = entry.is_root,
            .is_head = entry.is_head,
            .photo_changes = entry.photo_changes,
            .shared_grade_changes = entry.shared_grade_changes,
            .mask_changes = entry.mask_changes,
            .style_changes = entry.style_changes,
            .output_state_changes = entry.output_state_changes,
        });
    }
    return result;
}

BackendLibraryHistoryRefPage
library_history_ref_page(const shadow::desktop::FfiLibraryHistoryRefPage& source) {
    return {
        .refs = history_refs(source.refs, "library_history_ref_page"),
        .has_more = source.has_more,
        .next_cursor = qstring(source.next_cursor),
    };
}

} // namespace desktop_backend_projection

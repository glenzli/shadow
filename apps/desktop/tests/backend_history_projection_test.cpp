#include "backend/history_projection.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "History projection contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

shadow::desktop::FfiHistoryRef named_ref(const std::string& name, const std::string& commit_id) {
    shadow::desktop::FfiHistoryRef result;
    result.name = name;
    result.kind = shadow::desktop::FfiHistoryRefKind::NamedVersion;
    result.commit_id = commit_id;
    result.updated_at_ms = 1'234;
    return result;
}

} // namespace

int main() {
    shadow::desktop::FfiPhotoHistoryEntry photo_entry;
    photo_entry.commit_id = "recipe-2";
    photo_entry.name = "Second look";
    photo_entry.created_at_ms = 2'000;
    photo_entry.parent_commit_ids.push_back("recipe-1");
    photo_entry.refs.push_back(named_ref("versions/2", "recipe-2"));
    photo_entry.is_named = true;
    photo_entry.grade_nodes_modified = 2;
    photo_entry.changed_parameter_keys.push_back("exposure_stops");
    photo_entry.has_other_changes = true;

    shadow::desktop::FfiPhotoHistoryPage photo_page;
    photo_page.entries.push_back(std::move(photo_entry));
    photo_page.has_more = true;
    photo_page.next_cursor.created_at_ms = 2'000;
    photo_page.next_cursor.commit_id = "recipe-2";
    const auto projected_photo = desktop_backend_projection::photo_history_page(photo_page);
    require(projected_photo.entries.size() == 1, "photo row count is preserved");
    require(projected_photo.has_more, "photo continuation is preserved");
    require(
        projected_photo.next_cursor.commit_id == QStringLiteral("recipe-2"),
        "photo cursor identity is preserved"
    );
    const auto& photo = projected_photo.entries.constFirst();
    require(
        photo.name == QStringLiteral("Second look")
            && photo.parent_commit_ids == QStringList{QStringLiteral("recipe-1")}
            && photo.refs.constFirst().kind == BackendHistoryRefKind::NamedVersion
            && photo.grade_nodes_modified == 2
            && photo.changed_parameter_keys == QStringList{QStringLiteral("exposure_stops")}
            && photo.has_other_changes,
        "photo refs and semantic diff fields are lossless"
    );

    shadow::desktop::FfiLibraryHistoryEntry library_entry;
    library_entry.commit_id = "library-2";
    library_entry.message = "Second look";
    library_entry.created_at_ms = 2'100;
    library_entry.parent_commit_ids.push_back("library-1");
    library_entry.refs.push_back(named_ref("versions/library-2", "library-2"));
    library_entry.is_head = true;
    library_entry.photo_changes = 3;
    library_entry.shared_grade_changes = 1;
    library_entry.mask_changes = 2;
    library_entry.style_changes = 4;
    library_entry.output_state_changes = 5;

    shadow::desktop::FfiLibraryHistoryPage library_page;
    library_page.entries.push_back(std::move(library_entry));
    const auto projected_library = desktop_backend_projection::library_history_page(library_page);
    require(
        projected_library.entries.size() == 1 && projected_library.entries.constFirst().is_head
            && projected_library.entries.constFirst().photo_changes == 3
            && projected_library.entries.constFirst().output_state_changes == 5,
        "Library identity, head, and entity diff counts are lossless"
    );

    shadow::desktop::FfiLibraryHistoryRefPage ref_page;
    ref_page.refs.push_back(named_ref("versions/library-2", "library-2"));
    ref_page.has_more = true;
    ref_page.next_cursor = "versions/library-2";
    const auto projected_refs = desktop_backend_projection::library_history_ref_page(ref_page);
    require(
        projected_refs.refs.size() == 1 && projected_refs.has_more
            && projected_refs.next_cursor == QStringLiteral("versions/library-2"),
        "Library ref pagination is lossless"
    );

    const BackendHistoryCursor cursor{
        .created_at_ms = 99,
        .commit_id = QStringLiteral("cursor-id"),
    };
    const auto ffi_cursor = desktop_backend_projection::ffi_history_cursor(cursor);
    require(
        ffi_cursor.created_at_ms == 99 && std::string(ffi_cursor.commit_id) == "cursor-id",
        "Qt cursor maps back to the Rust ABI"
    );
    return EXIT_SUCCESS;
}

//! Actor-side execution for Library keywords.

use crate::Catalog;

use super::super::protocol::LibraryKeywordsMessage;

pub(super) fn run_library_keywords_message(catalog: &mut Catalog, message: LibraryKeywordsMessage) {
    match message {
        LibraryKeywordsMessage::Create(parent_id, name, now_ms, response) => {
            let _ = response.send(catalog.create_library_keyword(parent_id, &name, now_ms));
        }
        LibraryKeywordsMessage::Rename(keyword_id, name, now_ms, response) => {
            let _ = response.send(catalog.rename_library_keyword(keyword_id, &name, now_ms));
        }
        LibraryKeywordsMessage::Move(keyword_id, parent_id, now_ms, response) => {
            let _ = response.send(catalog.move_library_keyword(keyword_id, parent_id, now_ms));
        }
        LibraryKeywordsMessage::DeleteSubtree(keyword_id, response) => {
            let _ = response.send(catalog.delete_library_keyword_subtree(keyword_id));
        }
        LibraryKeywordsMessage::Tree(response) => {
            let _ = response.send(catalog.library_keyword_tree());
        }
        LibraryKeywordsMessage::ForPhoto(photo_id, response) => {
            let _ = response.send(catalog.library_keywords_for_photo(photo_id));
        }
        LibraryKeywordsMessage::Assign {
            keyword_id,
            photo_ids,
            origin,
            source_label,
            confidence_milli,
            now_ms,
            response,
        } => {
            let _ = response.send(catalog.assign_library_keyword_to_photos(
                keyword_id,
                &photo_ids,
                origin,
                &source_label,
                confidence_milli,
                now_ms,
            ));
        }
        LibraryKeywordsMessage::Remove(keyword_id, photo_ids, response) => {
            let _ =
                response.send(catalog.remove_library_keyword_from_photos(keyword_id, &photo_ids));
        }
    }
}

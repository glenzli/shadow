//! Actor-side execution for durable distinct-photo relationships.

use crate::Catalog;

use super::super::protocol::LibraryRelationshipsMessage;

pub(super) fn run_library_relationships_message(
    catalog: &mut Catalog,
    message: LibraryRelationshipsMessage,
) {
    match message {
        LibraryRelationshipsMessage::Create(request, response) => {
            let _ = response.send(catalog.create_photo_group(&request));
        }
        LibraryRelationshipsMessage::ReplaceMembers(
            group_id,
            photo_ids,
            anchor_photo_id,
            now_ms,
            response,
        ) => {
            let _ = response.send(catalog.replace_photo_group_members(
                group_id,
                &photo_ids,
                anchor_photo_id,
                now_ms,
            ));
        }
        LibraryRelationshipsMessage::Get(group_id, response) => {
            let _ = response.send(catalog.photo_group(group_id));
        }
        LibraryRelationshipsMessage::ForPhoto(photo_id, response) => {
            let _ = response.send(catalog.photo_groups_for_photo(photo_id));
        }
        LibraryRelationshipsMessage::Delete(group_id, response) => {
            let _ = response.send(catalog.delete_photo_group(group_id));
        }
    }
}

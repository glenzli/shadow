//! Actor-side execution for Library affinity state and collections.

use crate::Catalog;

use super::super::protocol::LibraryCollectionsMessage;

pub(super) fn run_library_collections_message(
    catalog: &mut Catalog,
    message: LibraryCollectionsMessage,
) {
    match message {
        LibraryCollectionsMessage::SetPhotoState(state, response) => {
            let _ = response.send(catalog.set_photo_library_state(state.as_ref()));
        }
        LibraryCollectionsMessage::PhotoState(photo_id, response) => {
            let _ = response.send(catalog.photo_library_state(photo_id));
        }
        LibraryCollectionsMessage::CreateAlbum(kind, name, query_json, now_ms, response) => {
            let _ = response.send(catalog.create_library_album(
                kind,
                &name,
                query_json.as_deref(),
                now_ms,
            ));
        }
        LibraryCollectionsMessage::CreateSmartAlbum(name, query, now_ms, response) => {
            let _ = response.send(catalog.create_smart_library_album(&name, &query, now_ms));
        }
        LibraryCollectionsMessage::RenameAlbum(album_id, name, now_ms, response) => {
            let _ = response.send(catalog.rename_library_album(album_id, &name, now_ms));
        }
        LibraryCollectionsMessage::ReplaceSmartAlbumQuery(album_id, query, now_ms, response) => {
            let _ = response.send(catalog.replace_smart_album_query(album_id, &query, now_ms));
        }
        LibraryCollectionsMessage::DeleteAlbum(album_id, response) => {
            let _ = response.send(catalog.delete_library_album(album_id));
        }
        LibraryCollectionsMessage::Albums(response) => {
            let _ = response.send(catalog.library_albums());
        }
        LibraryCollectionsMessage::AddPhoto(album_id, photo_id, sort_key, now_ms, response) => {
            let _ = response.send(catalog.add_photo_to_album(album_id, photo_id, sort_key, now_ms));
        }
        LibraryCollectionsMessage::RemovePhoto(album_id, photo_id, response) => {
            let _ = response.send(catalog.remove_photo_from_album(album_id, photo_id));
        }
        LibraryCollectionsMessage::AlbumsForPhoto(photo_id, response) => {
            let _ = response.send(catalog.albums_for_photo(photo_id));
        }
        LibraryCollectionsMessage::SmartAlbumFilter(album_id, response) => {
            let _ = response.send(catalog.smart_album_filter(album_id));
        }
        LibraryCollectionsMessage::SmartAlbumPhotoPage(
            album_id,
            after,
            requested_limit,
            response,
        ) => {
            let _ = response.send(catalog.smart_album_photo_page(
                album_id,
                after.as_ref(),
                requested_limit,
            ));
        }
        LibraryCollectionsMessage::SmartAlbumPhotoCount(album_id, response) => {
            let _ = response.send(catalog.smart_album_photo_count(album_id));
        }
    }
}

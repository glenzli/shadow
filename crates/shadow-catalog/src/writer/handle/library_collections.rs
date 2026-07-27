//! Client adapters for Library affinity state, albums, and membership.

use shadow_domain::{CollectionId, PhotoId};

use crate::{
    AlbumKind, AlbumRecord, CatalogError, LibraryPhotoCursor, LibraryPhotoFilter, LibraryPhotoPage,
    PhotoLibraryState, SetPhotoLibraryState, SmartAlbumQueryV1,
};

use super::super::{
    CatalogHandle,
    protocol::{LibraryCollectionsMessage, Message},
};

impl CatalogHandle {
    /// Sets an independent Library like/color state for a photo.
    pub fn set_photo_library_state(
        &self,
        state: &SetPhotoLibraryState,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::SetPhotoState(
                Box::new(state.clone()),
                response,
            ))
        })
    }

    pub fn photo_library_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryState, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::PhotoState(photo_id, response))
        })
    }

    pub fn create_library_album(
        &self,
        kind: AlbumKind,
        name: &str,
        query_json: Option<&str>,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::CreateAlbum(
                kind,
                name.to_owned(),
                query_json.map(str::to_owned),
                now_ms,
                response,
            ))
        })
    }

    /// Creates a smart album from the strict, version-one query contract.
    pub fn create_smart_library_album(
        &self,
        name: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::CreateSmartAlbum(
                name.to_owned(),
                query.clone(),
                now_ms,
                response,
            ))
        })
    }

    /// Renames a manual or smart album without changing its membership or query.
    pub fn rename_library_album(
        &self,
        album_id: CollectionId,
        name: &str,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::RenameAlbum(
                album_id,
                name.to_owned(),
                now_ms,
                response,
            ))
        })
    }

    /// Replaces the executable query of one existing smart album.
    pub fn replace_smart_album_query(
        &self,
        album_id: CollectionId,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::ReplaceSmartAlbumQuery(
                album_id,
                query.clone(),
                now_ms,
                response,
            ))
        })
    }

    /// Deletes one album and only that album's explicit memberships.
    pub fn delete_library_album(&self, album_id: CollectionId) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::DeleteAlbum(album_id, response))
        })
    }

    pub fn library_albums(&self) -> Result<Vec<AlbumRecord>, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::Albums(response))
        })
    }

    pub fn add_photo_to_album(
        &self,
        album_id: CollectionId,
        photo_id: PhotoId,
        sort_key: i64,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::AddPhoto(
                album_id, photo_id, sort_key, now_ms, response,
            ))
        })
    }

    pub fn remove_photo_from_album(
        &self,
        album_id: CollectionId,
        photo_id: PhotoId,
    ) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::RemovePhoto(
                album_id, photo_id, response,
            ))
        })
    }

    pub fn albums_for_photo(&self, photo_id: PhotoId) -> Result<Vec<AlbumRecord>, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::AlbumsForPhoto(
                photo_id, response,
            ))
        })
    }

    /// Resolves the executable, indexed filter behind one smart album.
    pub fn smart_album_filter(
        &self,
        album_id: CollectionId,
    ) -> Result<LibraryPhotoFilter, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::SmartAlbumFilter(
                album_id, response,
            ))
        })
    }

    /// Reads one bounded keyset page from a smart album.
    pub fn smart_album_photo_page(
        &self,
        album_id: CollectionId,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::SmartAlbumPhotoPage(
                album_id,
                after.cloned(),
                requested_limit,
                response,
            ))
        })
    }

    /// Counts one settled smart album through the actor.
    pub fn smart_album_photo_count(&self, album_id: CollectionId) -> Result<u64, CatalogError> {
        self.request(|response| {
            Message::LibraryCollections(LibraryCollectionsMessage::SmartAlbumPhotoCount(
                album_id, response,
            ))
        })
    }
}

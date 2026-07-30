//! Photo affinity state plus manual and smart Library collections.
//!
//! Collection persistence and membership live together because they form one user-visible
//! organization contract; photo-grid execution remains in `browse`.

use rusqlite::{OptionalExtension, params};
use shadow_domain::{CollectionId, EntityId, PhotoId};

use crate::{Catalog, CatalogError};

use super::{
    AlbumKind, AlbumRecord, LibraryPhotoCursor, LibraryPhotoFilter, LibraryPhotoPage,
    PhotoLibraryState, SetPhotoLibraryState, SmartAlbumQueryV1,
    integrity::{ensure_manual_album, ensure_photo_exists, ensure_photo_exists_connection},
    model::{validate_album, validate_library_state},
    rows::read_album,
};

impl Catalog {
    /// Replaces the independent like/color state for one photo.
    pub fn set_photo_library_state(
        &mut self,
        state: &SetPhotoLibraryState,
    ) -> Result<(), CatalogError> {
        validate_library_state(state)?;
        let transaction = self.connection.transaction()?;
        ensure_photo_exists(&transaction, state.photo_id)?;
        transaction.execute(
            "INSERT INTO photo_library_state(photo_id, liked, color_label, updated_at_ms)
             VALUES (?1, ?2, ?3, ?4)
             ON CONFLICT(photo_id) DO UPDATE SET
                 liked = excluded.liked,
                 color_label = excluded.color_label,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                state.photo_id.as_bytes().as_slice(),
                i64::from(state.liked),
                state.color_label.trim().to_ascii_lowercase(),
                state.updated_at_ms,
            ],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Reads current like/color state; absent rows are the neutral default.
    pub fn photo_library_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryState, CatalogError> {
        ensure_photo_exists_connection(&self.connection, photo_id)?;
        self.connection
            .query_row(
                "SELECT liked, color_label, updated_at_ms
                 FROM photo_library_state WHERE photo_id = ?1",
                [photo_id.as_bytes().as_slice()],
                |row| {
                    Ok(PhotoLibraryState {
                        photo_id,
                        liked: row.get::<_, i64>(0)? != 0,
                        color_label: row.get(1)?,
                        updated_at_ms: row.get(2)?,
                    })
                },
            )
            .optional()?
            .map_or_else(
                || {
                    Ok(PhotoLibraryState {
                        photo_id,
                        liked: false,
                        color_label: "none".to_owned(),
                        updated_at_ms: 0,
                    })
                },
                Ok,
            )
    }

    /// Creates one user-visible album. A photo may belong to any number of
    /// manual albums through [`Self::add_photo_to_album`].
    pub fn create_library_album(
        &mut self,
        kind: AlbumKind,
        name: &str,
        query_json: Option<&str>,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let (name, query_json) = validate_album(kind, name, query_json)?;
        let id = CollectionId::new_v7();
        self.connection.execute(
            "INSERT INTO library_albums(id, kind, name, query_json, created_at_ms, updated_at_ms)
             VALUES (?1, ?2, ?3, ?4, ?5, ?5)",
            params![
                id.as_bytes().as_slice(),
                kind.as_str(),
                name,
                query_json.as_deref(),
                now_ms,
            ],
        )?;
        Ok(AlbumRecord {
            id,
            kind,
            name,
            query_json,
            created_at_ms: now_ms,
            updated_at_ms: now_ms,
        })
    }

    /// Creates a smart album from the current, strict v1 query contract.
    pub fn create_smart_library_album(
        &mut self,
        name: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let query_json = query.to_json()?;
        self.create_library_album(AlbumKind::Smart, name, Some(&query_json), now_ms)
    }

    /// Lists albums in stable case-insensitive name order.
    pub fn library_albums(&self) -> Result<Vec<AlbumRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, kind, name, query_json, created_at_ms, updated_at_ms
             FROM library_albums ORDER BY name COLLATE NOCASE, id",
        )?;
        let rows = statement.query_map([], read_album)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Renames one manual or smart album without changing its membership or
    /// query. Smart queries are re-canonicalized while validating the update.
    pub fn rename_library_album(
        &mut self,
        album_id: CollectionId,
        name: &str,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let existing = self.library_album_by_id(album_id)?;
        let (name, query_json) =
            validate_album(existing.kind, name, existing.query_json.as_deref())?;
        self.connection.execute(
            "UPDATE library_albums
             SET name = ?2, query_json = ?3, updated_at_ms = ?4
             WHERE id = ?1",
            params![
                album_id.as_bytes().as_slice(),
                name,
                query_json.as_deref(),
                now_ms,
            ],
        )?;
        Ok(AlbumRecord {
            id: album_id,
            kind: existing.kind,
            name,
            query_json,
            created_at_ms: existing.created_at_ms,
            updated_at_ms: now_ms,
        })
    }

    /// Replaces the query of a smart album. Manual albums cannot acquire a
    /// query later: that keeps explicit membership and computed membership
    /// unambiguous in both the UI and the catalog.
    pub fn replace_smart_album_query(
        &mut self,
        album_id: CollectionId,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let existing = self.library_album_by_id(album_id)?;
        if existing.kind != AlbumKind::Smart {
            return Err(CatalogError::InvalidAlbum(
                "manual albums cannot be given a smart query".into(),
            ));
        }
        let query_json = query.to_json()?;
        self.connection.execute(
            "UPDATE library_albums SET query_json = ?2, updated_at_ms = ?3 WHERE id = ?1",
            params![album_id.as_bytes().as_slice(), query_json.as_str(), now_ms],
        )?;
        Ok(AlbumRecord {
            id: album_id,
            kind: AlbumKind::Smart,
            name: existing.name,
            query_json: Some(query_json),
            created_at_ms: existing.created_at_ms,
            updated_at_ms: now_ms,
        })
    }

    /// Deletes one album. `SQLite` cascades only that album's explicit manual
    /// memberships; it never deletes a photo, source, or another album.
    pub fn delete_library_album(&mut self, album_id: CollectionId) -> Result<bool, CatalogError> {
        let deleted = self.connection.execute(
            "DELETE FROM library_albums WHERE id = ?1",
            [album_id.as_bytes().as_slice()],
        )?;
        Ok(deleted != 0)
    }

    /// Resolves the query behind one smart album. Manual albums deliberately
    /// remain membership-backed and must be queried with `album_id` instead.
    pub fn smart_album_filter(
        &self,
        album_id: CollectionId,
    ) -> Result<LibraryPhotoFilter, CatalogError> {
        let album = self.library_album_by_id(album_id)?;
        match (album.kind, album.query_json.as_deref()) {
            (AlbumKind::Smart, Some(query_json)) => {
                SmartAlbumQueryV1::from_json(query_json)?.library_filter()
            }
            (AlbumKind::Smart, None) => Err(CatalogError::InvalidAlbum(
                "persisted smart album is missing its query".into(),
            )),
            (AlbumKind::Manual, _) => Err(CatalogError::InvalidAlbum(
                "manual albums do not have a smart query".into(),
            )),
        }
    }

    fn library_album_by_id(&self, album_id: CollectionId) -> Result<AlbumRecord, CatalogError> {
        self.connection
            .query_row(
                "SELECT id, kind, name, query_json, created_at_ms, updated_at_ms
                 FROM library_albums WHERE id = ?1",
                [album_id.as_bytes().as_slice()],
                read_album,
            )
            .optional()?
            .ok_or(CatalogError::AlbumNotFound(album_id))
    }

    /// Pages one smart album through the normal keyset grid implementation.
    pub fn smart_album_photo_page(
        &self,
        album_id: CollectionId,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        let filter = self.smart_album_filter(album_id)?;
        self.library_photo_page(
            &filter,
            super::LibraryPhotoOrder::default(),
            after,
            requested_limit,
        )
    }

    /// Counts one settled smart album through the normal indexed filter path.
    pub fn smart_album_photo_count(&self, album_id: CollectionId) -> Result<u64, CatalogError> {
        let filter = self.smart_album_filter(album_id)?;
        self.library_photo_count(&filter)
    }

    /// Adds a photo to a manual album. Repeating the same operation is
    /// idempotent and preserves its original order.
    pub fn add_photo_to_album(
        &mut self,
        album_id: CollectionId,
        photo_id: PhotoId,
        sort_key: i64,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        ensure_photo_exists(&transaction, photo_id)?;
        ensure_manual_album(&transaction, album_id)?;
        transaction.execute(
            "INSERT INTO library_album_memberships(album_id, photo_id, added_at_ms, sort_key)
             VALUES (?1, ?2, ?3, ?4) ON CONFLICT(album_id, photo_id) DO NOTHING",
            params![
                album_id.as_bytes().as_slice(),
                photo_id.as_bytes().as_slice(),
                now_ms,
                sort_key,
            ],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Removes the membership from one album only; it never removes the photo
    /// from the Library or any other album.
    pub fn remove_photo_from_album(
        &mut self,
        album_id: CollectionId,
        photo_id: PhotoId,
    ) -> Result<bool, CatalogError> {
        let removed = self.connection.execute(
            "DELETE FROM library_album_memberships WHERE album_id = ?1 AND photo_id = ?2",
            params![
                album_id.as_bytes().as_slice(),
                photo_id.as_bytes().as_slice()
            ],
        )?;
        Ok(removed != 0)
    }

    /// Lists the explicit manual-album memberships for one photo.
    ///
    /// Smart albums are computed from their query at browse time, so they do
    /// not create mutable membership rows and are intentionally absent here.
    pub fn albums_for_photo(&self, photo_id: PhotoId) -> Result<Vec<AlbumRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT a.id, a.kind, a.name, a.query_json, a.created_at_ms, a.updated_at_ms
             FROM library_albums a
             JOIN library_album_memberships m ON m.album_id = a.id
             WHERE m.photo_id = ?1
             ORDER BY a.name COLLATE NOCASE, a.id",
        )?;
        let rows = statement.query_map([photo_id.as_bytes().as_slice()], read_album)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }
}

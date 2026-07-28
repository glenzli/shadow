//! Durable Library albums, memberships, and photo-affinity state.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{AlbumKind, AlbumRecord, SetPhotoLibraryState, SmartAlbumQueryV1};
use shadow_domain::PhotoId;

use crate::{ffi, review_service::ReviewService};

use super::{
    LibraryService,
    query_contract::{
        empty_ffi_library_filter, ffi_library_filter, library_album_id_from_text,
        library_cursor_from_ffi, smart_album_query_from_ffi,
    },
};

impl LibraryService {
    /// Presents every durable album to the CXX facade without exposing raw
    /// stored JSON. Smart queries round-trip through their typed v1 filter.
    pub(crate) fn ffi_albums(&self) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
        ffi_library_albums(self.albums()?)
    }

    /// Lists the durable user albums. The desktop uses this for its Library
    /// sidebar; it never infers albums from import directories.
    pub(crate) fn albums(&self) -> AnyResult<Vec<AlbumRecord>> {
        Ok(self.catalog.library_albums()?)
    }

    /// Creates one membership-backed manual album.
    pub(crate) fn create_manual_album(&self, name: &str, now_ms: i64) -> AnyResult<AlbumRecord> {
        Ok(self
            .catalog
            .create_library_album(AlbumKind::Manual, name, None, now_ms)?)
    }

    pub(crate) fn create_manual_album_ffi(
        &self,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        ffi_library_album(self.create_manual_album(name, now_ms)?)
    }

    /// Creates one query-backed smart album from the typed v1 contract.
    pub(crate) fn create_smart_album(
        &self,
        name: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> AnyResult<AlbumRecord> {
        Ok(self
            .catalog
            .create_smart_library_album(name, query, now_ms)?)
    }

    pub(crate) fn create_smart_album_ffi(
        &self,
        name: &str,
        query_filter: &ffi::FfiLibraryPhotoFilter,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        let query = smart_album_query_from_ffi(query_filter)?;
        ffi_library_album(self.create_smart_album(name, &query, now_ms)?)
    }

    pub(crate) fn rename_album(
        &self,
        album_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<AlbumRecord> {
        Ok(self.catalog.rename_library_album(
            library_album_id_from_text(album_id)?,
            name,
            now_ms,
        )?)
    }

    pub(crate) fn rename_album_ffi(
        &self,
        album_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        ffi_library_album(self.rename_album(album_id, name, now_ms)?)
    }

    pub(crate) fn replace_smart_album_query(
        &self,
        album_id: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> AnyResult<AlbumRecord> {
        Ok(self.catalog.replace_smart_album_query(
            library_album_id_from_text(album_id)?,
            query,
            now_ms,
        )?)
    }

    pub(crate) fn replace_smart_album_query_ffi(
        &self,
        album_id: &str,
        query_filter: &ffi::FfiLibraryPhotoFilter,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        let query = smart_album_query_from_ffi(query_filter)?;
        ffi_library_album(self.replace_smart_album_query(album_id, &query, now_ms)?)
    }

    /// Deletes an album without deleting any photo it previously contained.
    pub(crate) fn delete_album(&self, album_id: &str) -> AnyResult<bool> {
        Ok(self
            .catalog
            .delete_library_album(library_album_id_from_text(album_id)?)?)
    }

    /// Adds a photo to one manual album. The Catalog rejects a smart album so
    /// callers cannot accidentally turn a computed album into a static one.
    pub(crate) fn add_photo_to_manual_album(
        &self,
        album_id: &str,
        photo_id: &str,
        sort_key: i64,
        now_ms: i64,
    ) -> AnyResult<()> {
        self.catalog.add_photo_to_album(
            library_album_id_from_text(album_id)?,
            library_photo_id_from_text(photo_id)?,
            sort_key,
            now_ms,
        )?;
        Ok(())
    }

    pub(crate) fn remove_photo_from_manual_album(
        &self,
        album_id: &str,
        photo_id: &str,
    ) -> AnyResult<bool> {
        Ok(self.catalog.remove_photo_from_album(
            library_album_id_from_text(album_id)?,
            library_photo_id_from_text(photo_id)?,
        )?)
    }

    pub(crate) fn albums_for_photo(&self, photo_id: &str) -> AnyResult<Vec<AlbumRecord>> {
        Ok(self
            .catalog
            .albums_for_photo(library_photo_id_from_text(photo_id)?)?)
    }

    pub(crate) fn ffi_albums_for_photo(
        &self,
        photo_id: &str,
    ) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
        ffi_library_albums(self.albums_for_photo(photo_id)?)
    }

    /// Renders a smart album through the same cached-artifact presentation
    /// path as an ordinary Library page.
    pub(crate) fn smart_album_photo_page(
        &self,
        review: &ReviewService,
        album_id: &str,
        ffi_cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        let album_id = library_album_id_from_text(album_id)?;
        let cursor = library_cursor_from_ffi(ffi_cursor)?;
        let page = self.catalog.smart_album_photo_page(
            album_id,
            cursor.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
        )?;
        self.present_photo_page(review, page)
    }

    pub(crate) fn smart_album_photo_count(&self, album_id: &str) -> AnyResult<u64> {
        Ok(self
            .catalog
            .smart_album_photo_count(library_album_id_from_text(album_id)?)?)
    }

    /// Replaces only the mutable Library-affinity projection for one logical
    /// photo. Review flags and ratings deliberately use the separate,
    /// append-only decision service.
    pub(crate) fn set_photo_library_state(
        &self,
        photo_id: &str,
        liked: bool,
        color_label: &str,
        updated_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoLibraryState> {
        let photo_id = photo_id
            .parse::<PhotoId>()
            .with_context(|| format!("parse Library photo id {photo_id}"))?;
        self.catalog
            .set_photo_library_state(&SetPhotoLibraryState {
                photo_id,
                liked,
                color_label: color_label.to_owned(),
                updated_at_ms,
            })?;
        let state = self.catalog.photo_library_state(photo_id)?;
        Ok(ffi::FfiPhotoLibraryState {
            photo_id: state.photo_id.to_string(),
            liked: state.liked,
            color_label: state.color_label,
            updated_at_ms: state.updated_at_ms,
        })
    }
}

fn ffi_library_albums(albums: Vec<AlbumRecord>) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
    albums.into_iter().map(ffi_library_album).collect()
}

fn ffi_library_album(album: AlbumRecord) -> AnyResult<ffi::FfiLibraryAlbum> {
    let (kind, query_filter) = match (album.kind, album.query_json.as_deref()) {
        (AlbumKind::Manual, None) => (ffi::FfiLibraryAlbumKind::Manual, empty_ffi_library_filter()),
        (AlbumKind::Manual, Some(_)) => bail!("manual album {} unexpectedly has a query", album.id),
        (AlbumKind::Smart, Some(query_json)) => {
            let query = SmartAlbumQueryV1::from_json(query_json)?;
            (
                ffi::FfiLibraryAlbumKind::Smart,
                ffi_library_filter(query.library_filter()?),
            )
        }
        (AlbumKind::Smart, None) => bail!("smart album {} is missing its query", album.id),
    };
    Ok(ffi::FfiLibraryAlbum {
        id: album.id.to_string(),
        kind,
        name: album.name,
        query_filter,
        created_at_ms: album.created_at_ms,
        updated_at_ms: album.updated_at_ms,
    })
}

pub(super) fn library_photo_id_from_text(photo_id: &str) -> AnyResult<PhotoId> {
    let photo_id = photo_id.trim();
    if photo_id.is_empty() {
        bail!("Library photo id is required");
    }
    photo_id
        .parse::<PhotoId>()
        .with_context(|| format!("parse Library photo id {photo_id}"))
}

#[cfg(test)]
mod tests;

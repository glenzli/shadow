//! Semantic integrity checks shared by Library write transactions.

use rusqlite::{OptionalExtension, Transaction};
use shadow_domain::{CollectionId, EntityId, PhotoId, RepresentationId};

use crate::{CatalogError, row_codec::read_id};

pub(super) fn ensure_photo_exists(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let exists: Option<i64> = transaction
        .query_row(
            "SELECT 1 FROM photos WHERE id = ?1",
            [photo_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?;
    if exists.is_none() {
        return Err(CatalogError::PhotoNotFound(photo_id));
    }
    Ok(())
}

pub(super) fn ensure_photo_exists_connection(
    connection: &rusqlite::Connection,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let exists: Option<i64> = connection
        .query_row(
            "SELECT 1 FROM photos WHERE id = ?1",
            [photo_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?;
    if exists.is_none() {
        return Err(CatalogError::PhotoNotFound(photo_id));
    }
    Ok(())
}

pub(super) fn ensure_representation_belongs_to_photo(
    transaction: &Transaction<'_>,
    representation_id: RepresentationId,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let owner: Option<PhotoId> = transaction
        .query_row(
            "SELECT photo_id FROM representations WHERE id = ?1",
            [representation_id.as_bytes().as_slice()],
            |row| read_id(row, 0),
        )
        .optional()?;
    match owner {
        Some(owner) if owner == photo_id => Ok(()),
        Some(_) => Err(CatalogError::InvalidLibraryFacts(
            "indexed representation belongs to another photo".into(),
        )),
        None => Err(CatalogError::RepresentationNotFound(representation_id)),
    }
}

pub(super) fn ensure_manual_album(
    transaction: &Transaction<'_>,
    album_id: CollectionId,
) -> Result<(), CatalogError> {
    let kind: Option<String> = transaction
        .query_row(
            "SELECT kind FROM library_albums WHERE id = ?1",
            [album_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?;
    match kind.as_deref() {
        Some("manual") => Ok(()),
        Some("smart") => Err(CatalogError::InvalidAlbum(
            "photos cannot be manually inserted into a smart album".into(),
        )),
        Some(other) => Err(CatalogError::InvalidAlbum(format!(
            "unknown persisted album kind {other:?}"
        ))),
        None => Err(CatalogError::AlbumNotFound(album_id)),
    }
}

//! Idempotent asset registration and in-place source revision invalidation.

use rusqlite::{OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{
    AssetLocation, EntityId, LocationId, LocationStatus, PhotoId, RepresentationId,
    RepresentationKind,
};

use crate::{Catalog, CatalogError, row_codec::read_id};

/// Produces the persisted, case-insensitive key used by Library name sorting.
///
/// `display_path` is presentation text and may use either native separator,
/// even when a catalog is opened on another platform. Persisting the leaf key
/// keeps name pagination indexed instead of splitting every path per page.
pub(crate) fn location_file_name_sort_key(display_path: &str) -> String {
    display_path
        .trim_end_matches(['/', '\\'])
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(display_path)
        .to_lowercase()
}

#[derive(Debug, Clone)]
pub struct RegisterAsset {
    pub kind: RepresentationKind,
    pub location: AssetLocation,
    pub byte_len: u64,
    pub modified_at_ms: Option<i64>,
    pub now_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RegisteredAsset {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location_id: LocationId,
    pub status: RegistrationStatus,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RegistrationStatus {
    Inserted,
    Unchanged,
    NeedsRevalidation,
}

#[derive(Debug)]
pub(crate) struct ExistingAsset {
    pub(crate) photo_id: PhotoId,
    pub(crate) representation_id: RepresentationId,
    pub(crate) location_id: LocationId,
    pub(crate) byte_len: u64,
    pub(crate) modified_at_ms: Option<i64>,
}

impl Catalog {
    /// Registers an asset path atomically and idempotently.
    ///
    /// A path whose size or modification time changed is marked for later
    /// content revalidation instead of silently replacing its representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the registration transaction cannot be
    /// queried, written, or committed.
    pub fn register_asset(
        &mut self,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        let transaction = self.connection.transaction()?;
        let result = register_asset_in_transaction(&transaction, request)?;
        transaction.commit()?;
        Ok(result)
    }
}

pub(crate) fn register_asset_in_transaction(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
) -> rusqlite::Result<RegisteredAsset> {
    let existing = find_existing_asset(transaction, &request.location)?;

    if let Some(existing) = existing {
        let unchanged = existing.byte_len == request.byte_len
            && existing.modified_at_ms == request.modified_at_ms;

        if !unchanged {
            let byte_len = i64::try_from(request.byte_len)
                .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;
            // A location may be overwritten in place. Its representation
            // keeps the logical photo identity, but every byte-derived proof
            // belongs to the old revision and must be discarded atomically.
            // Updating the representation fingerprint also naturally
            // invalidates decoder snapshots and cache artifacts keyed by it.
            transaction.execute(
                "UPDATE representations SET byte_len = ?2, modified_at_ms = ?3 WHERE id = ?1",
                params![
                    existing.representation_id.as_bytes().as_slice(),
                    byte_len,
                    request.modified_at_ms,
                ],
            )?;
            transaction.execute(
                "DELETE FROM representation_content_identities WHERE representation_id = ?1",
                [existing.representation_id.as_bytes().as_slice()],
            )?;
            transaction.execute(
                "UPDATE locations SET status = ?1 WHERE id = ?2",
                params![
                    LocationStatus::NeedsRevalidation.as_str(),
                    existing.location_id.as_bytes().as_slice()
                ],
            )?;
        }

        Ok(RegisteredAsset {
            photo_id: existing.photo_id,
            representation_id: existing.representation_id,
            location_id: existing.location_id,
            status: if unchanged {
                RegistrationStatus::Unchanged
            } else {
                RegistrationStatus::NeedsRevalidation
            },
        })
    } else {
        insert_asset(transaction, request)
    }
}

pub(crate) fn find_existing_asset(
    transaction: &Transaction<'_>,
    location: &AssetLocation,
) -> rusqlite::Result<Option<ExistingAsset>> {
    transaction
        .query_row(
            "SELECT p.id, r.id, l.id, r.byte_len, r.modified_at_ms
             FROM locations l
             JOIN representations r ON r.id = l.representation_id
             JOIN photos p ON p.id = r.photo_id
             WHERE l.platform = ?1 AND l.native_path = ?2",
            params![location.platform.as_str(), location.native_path],
            |row| {
                let byte_len: i64 = row.get(3)?;
                Ok(ExistingAsset {
                    photo_id: read_id(row, 0)?,
                    representation_id: read_id(row, 1)?,
                    location_id: read_id(row, 2)?,
                    byte_len: u64::try_from(byte_len).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(3, Type::Integer, Box::new(error))
                    })?,
                    modified_at_ms: row.get(4)?,
                })
            },
        )
        .optional()
}

pub(crate) fn insert_asset(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
) -> rusqlite::Result<RegisteredAsset> {
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let location_id = LocationId::new_v7();
    let byte_len = i64::try_from(request.byte_len)
        .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;

    transaction.execute(
        "INSERT INTO photos(id, created_at_ms) VALUES (?1, ?2)",
        params![photo_id.as_bytes().as_slice(), request.now_ms],
    )?;
    transaction.execute(
        "INSERT INTO representations(
             id, photo_id, kind, byte_len, modified_at_ms, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            representation_id.as_bytes().as_slice(),
            photo_id.as_bytes().as_slice(),
            request.kind.as_str(),
            byte_len,
            request.modified_at_ms,
            request.now_ms
        ],
    )?;
    transaction.execute(
        "INSERT INTO locations(
             id, representation_id, platform, native_path, display_path, sort_name_key,
             status, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![
            location_id.as_bytes().as_slice(),
            representation_id.as_bytes().as_slice(),
            request.location.platform.as_str(),
            request.location.native_path,
            request.location.display_path,
            location_file_name_sort_key(&request.location.display_path),
            LocationStatus::Online.as_str(),
            request.now_ms
        ],
    )?;

    Ok(RegisteredAsset {
        photo_id,
        representation_id,
        location_id,
        status: RegistrationStatus::Inserted,
    })
}

#[cfg(test)]
mod tests;

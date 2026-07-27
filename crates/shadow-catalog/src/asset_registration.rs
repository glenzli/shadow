//! Idempotent asset registration and in-place source revision invalidation.

use rusqlite::{OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{
    AssetLocation, EntityId, LocationId, LocationStatus, PhotoId, RepresentationId,
    RepresentationKind,
};

use crate::{Catalog, CatalogError, row_codec::read_id};

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
    photo_id: PhotoId,
    representation_id: RepresentationId,
    location_id: LocationId,
    byte_len: u64,
    modified_at_ms: Option<i64>,
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
             id, representation_id, platform, native_path, display_path, status, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![
            location_id.as_bytes().as_slice(),
            representation_id.as_bytes().as_slice(),
            request.location.platform.as_str(),
            request.location.native_path,
            request.location.display_path,
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
mod tests {
    use super::*;
    use shadow_domain::Platform;

    fn request(byte_len: u64, modified_at_ms: Option<i64>) -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/DSC_0001.NEF".to_vec(),
                "/photos/DSC_0001.NEF",
            ),
            byte_len,
            modified_at_ms,
            now_ms: 1_700_000_000_000,
        }
    }

    #[test]
    fn registering_the_same_location_is_idempotent() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let first = catalog
            .register_asset(&request(42, Some(100)))
            .expect("first registration");
        let second = catalog
            .register_asset(&request(42, Some(100)))
            .expect("second registration");

        assert_eq!(first.status, RegistrationStatus::Inserted);
        assert_eq!(second.status, RegistrationStatus::Unchanged);
        assert_eq!(first.photo_id, second.photo_id);
        assert_eq!(catalog.stats().expect("stats").photos, 1);
    }

    #[test]
    fn changed_file_is_marked_for_revalidation_without_silent_replacement() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        catalog
            .register_asset(&request(42, Some(100)))
            .expect("initial registration");
        let changed = catalog
            .register_asset(&request(84, Some(200)))
            .expect("changed registration");

        assert_eq!(changed.status, RegistrationStatus::NeedsRevalidation);
        assert_eq!(
            catalog
                .stats()
                .expect("stats")
                .locations_needing_revalidation,
            1
        );
    }
}

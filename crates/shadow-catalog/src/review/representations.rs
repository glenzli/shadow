//! Bounded representation inventory for one logical photo.

use rusqlite::types::Type;
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use crate::{
    Catalog, CatalogError, RepresentationFingerprint,
    library::{platform_from_text, representation_kind_from_text},
    row_codec::read_id,
};

use super::PhotoRepresentationRecord;

impl Catalog {
    /// Lists original RAW/raster representations of one logical photo in
    /// stable edit preference order, including offline-only representations.
    pub fn photo_representations(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<PhotoRepresentationRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT r.photo_id, r.id, r.kind, r.byte_len, r.modified_at_ms,
                    l.platform, l.native_path, l.display_path,
                    (SELECT COUNT(*) FROM locations all_locations
                     WHERE all_locations.representation_id = r.id),
                    (SELECT COUNT(*) FROM locations online_locations
                     WHERE online_locations.representation_id = r.id
                       AND online_locations.status = 'online')
             FROM representations r
             JOIN locations l ON l.id = (
                 SELECT candidate.id FROM locations candidate
                 WHERE candidate.representation_id = r.id
                 ORDER BY CASE candidate.status WHEN 'online' THEN 0 ELSE 1 END,
                          candidate.created_at_ms DESC, candidate.id DESC
                 LIMIT 1
             )
             WHERE r.photo_id = ?1
               AND r.kind IN ('original_raw', 'original_raster')
             ORDER BY CASE r.kind WHEN 'original_raw' THEN 0 ELSE 1 END,
                      r.created_at_ms DESC, r.id DESC",
        )?;
        let rows = statement.query_map([photo_id.as_bytes().as_slice()], |row| {
            let byte_len: i64 = row.get(3)?;
            let location_count: i64 = row.get(8)?;
            let online_location_count: i64 = row.get(9)?;
            Ok(PhotoRepresentationRecord {
                photo_id: read_id(row, 0)?,
                representation_id: read_id(row, 1)?,
                kind: representation_kind_from_text(&row.get::<_, String>(2)?, 2)?,
                location: shadow_domain::AssetLocation::new(
                    platform_from_text(&row.get::<_, String>(5)?, 5)?,
                    row.get(6)?,
                    row.get::<_, String>(7)?,
                ),
                source: RepresentationFingerprint {
                    byte_len: u64::try_from(byte_len).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(3, Type::Integer, Box::new(error))
                    })?,
                    modified_at_ms: row.get(4)?,
                },
                location_count: u32::try_from(location_count).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(8, Type::Integer, Box::new(error))
                })?,
                online_location_count: u32::try_from(online_location_count).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(9, Type::Integer, Box::new(error))
                })?,
            })
        })?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Resolves one exact original representation owned by a logical photo.
    pub fn photo_representation(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<Option<PhotoRepresentationRecord>, CatalogError> {
        Ok(self
            .photo_representations(photo_id)?
            .into_iter()
            .find(|record| record.representation_id == representation_id))
    }
}

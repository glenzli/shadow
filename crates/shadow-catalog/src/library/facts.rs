//! Indexed metadata facts and their source-provenance transaction.

use rusqlite::{OptionalExtension, Transaction, params};
use shadow_domain::{EntityId, PhotoId};

use crate::{Catalog, CatalogError};

use super::{
    LibraryPhotoFacts,
    integrity::{ensure_photo_exists, ensure_representation_belongs_to_photo},
    metadata_overrides::refresh_effective_photo_library_facts,
    model::{normalized_equipment_key, validate_facts},
    rows::read_library_facts,
};

impl Catalog {
    /// Persists filterable metadata as a compact, indexed read model.
    pub fn upsert_photo_library_facts(
        &mut self,
        facts: &LibraryPhotoFacts,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        upsert_photo_library_facts_in_transaction(&transaction, facts)?;
        transaction.commit()?;
        Ok(())
    }

    /// Returns a photo's indexed Library facts when metadata has been decoded.
    pub fn photo_library_facts(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<LibraryPhotoFacts>, CatalogError> {
        self.connection
            .query_row(
                "SELECT photo_id, captured_at_unix_seconds, capture_day,
                        camera_make, camera_model, lens_make, lens_model,
                        aperture_milli, focal_length_tenth_mm, iso_speed,
                        latitude_e7, longitude_e7, place_name,
                        indexed_representation_id, indexed_source_byte_len,
                        indexed_source_modified_at_ms, indexed_at_ms
                 FROM photo_library_facts WHERE photo_id = ?1",
                [photo_id.as_bytes().as_slice()],
                read_library_facts,
            )
            .optional()
            .map_err(Into::into)
    }
}

pub(crate) fn upsert_photo_library_facts_in_transaction(
    transaction: &Transaction<'_>,
    facts: &LibraryPhotoFacts,
) -> Result<(), CatalogError> {
    validate_facts(facts)?;
    ensure_photo_exists(transaction, facts.photo_id)?;
    if let Some(representation_id) = facts.indexed_representation_id {
        ensure_representation_belongs_to_photo(transaction, representation_id, facts.photo_id)?;
    }
    let source_byte_len = facts
        .indexed_source
        .map(|source| i64::try_from(source.byte_len))
        .transpose()
        .map_err(|error| CatalogError::InvalidLibraryFacts(error.to_string()))?;
    let indexed_representation_bytes = facts
        .indexed_representation_id
        .map(|value| value.as_bytes().to_vec());
    transaction.execute(
        "INSERT INTO photo_library_facts(
             photo_id, captured_at_unix_seconds, capture_day,
             camera_make, camera_model, camera_key,
             lens_make, lens_model, lens_key,
             aperture_milli, focal_length_tenth_mm, iso_speed,
             latitude_e7, longitude_e7, place_name,
             indexed_representation_id, indexed_source_byte_len,
             indexed_source_modified_at_ms, indexed_at_ms
         ) VALUES (
             ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9,
             ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19
         ) ON CONFLICT(photo_id) DO UPDATE SET
             captured_at_unix_seconds = excluded.captured_at_unix_seconds,
             capture_day = excluded.capture_day,
             camera_make = excluded.camera_make,
             camera_model = excluded.camera_model,
             camera_key = excluded.camera_key,
             lens_make = excluded.lens_make,
             lens_model = excluded.lens_model,
             lens_key = excluded.lens_key,
             aperture_milli = excluded.aperture_milli,
             focal_length_tenth_mm = excluded.focal_length_tenth_mm,
             iso_speed = excluded.iso_speed,
             latitude_e7 = excluded.latitude_e7,
             longitude_e7 = excluded.longitude_e7,
             place_name = excluded.place_name,
             indexed_representation_id = excluded.indexed_representation_id,
             indexed_source_byte_len = excluded.indexed_source_byte_len,
             indexed_source_modified_at_ms = excluded.indexed_source_modified_at_ms,
             indexed_at_ms = excluded.indexed_at_ms",
        params![
            facts.photo_id.as_bytes().as_slice(),
            facts.captured_at_unix_seconds,
            facts.capture_day.trim(),
            facts.camera_make.trim(),
            facts.camera_model.trim(),
            normalized_equipment_key(&facts.camera_make, &facts.camera_model),
            facts.lens_make.trim(),
            facts.lens_model.trim(),
            normalized_equipment_key(&facts.lens_make, &facts.lens_model),
            facts.aperture_milli.map(i64::from),
            facts.focal_length_tenth_mm.map(i64::from),
            facts.iso_speed,
            facts.latitude_e7.map(i64::from),
            facts.longitude_e7.map(i64::from),
            facts.place_name.trim(),
            indexed_representation_bytes.as_deref(),
            source_byte_len,
            facts
                .indexed_source
                .and_then(|source| source.modified_at_ms),
            facts.indexed_at_ms,
        ],
    )?;
    refresh_effective_photo_library_facts(transaction, facts.photo_id)?;
    Ok(())
}

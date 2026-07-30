//! Non-destructive user corrections layered over decoder-observed Library metadata.
//!
//! Decoder facts remain immutable observations. This owner stores only explicit
//! overrides and refreshes the indexed effective projection consumed by Library
//! browsing, so rescans cannot erase user intent and hot queries keep using
//! ordinary indexes.

use std::collections::HashSet;

use rusqlite::{OptionalExtension, Row, Transaction, params};
use shadow_domain::{EntityId, PhotoId};

use crate::{Catalog, CatalogError, library_metadata::capture_day};

use super::{
    LibraryCoordinates, LibraryMetadataOverride, LibraryMetadataOverrideAction,
    LibraryMetadataOverrideOrigin, LibraryPhotoFacts, PhotoLibraryMetadataOverrides,
    SetPhotoLibraryMetadataOverrides,
    integrity::{ensure_photo_exists, ensure_photo_exists_connection},
    rows::read_library_facts,
};

impl Catalog {
    pub fn set_photo_library_metadata_overrides(
        &mut self,
        command: &SetPhotoLibraryMetadataOverrides,
    ) -> Result<(), CatalogError> {
        self.set_photo_library_metadata_overrides_batch(std::slice::from_ref(command))
    }

    pub fn set_photo_library_metadata_overrides_batch(
        &mut self,
        commands: &[SetPhotoLibraryMetadataOverrides],
    ) -> Result<(), CatalogError> {
        if commands.len() > 100_000 {
            return Err(invalid("metadata override batch exceeds 100000 photos"));
        }
        let mut photo_ids = HashSet::with_capacity(commands.len());
        for command in commands {
            validate_command(command)?;
            if !photo_ids.insert(command.photo_id) {
                return Err(invalid("metadata override batch repeats a photo"));
            }
        }
        let transaction = self.connection.transaction()?;
        for command in commands {
            set_metadata_overrides_in_transaction(&transaction, command)?;
        }
        transaction.commit()?;
        Ok(())
    }

    pub fn photo_library_metadata_overrides(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryMetadataOverrides, CatalogError> {
        ensure_photo_exists_connection(&self.connection, photo_id)?;
        self.connection
            .query_row(
                "SELECT photo_id,
                        capture_time_mode, captured_at_unix_seconds,
                        capture_time_origin, capture_time_source_label,
                        capture_time_updated_at_ms,
                        coordinates_mode, latitude_e7, longitude_e7, place_name,
                        coordinates_origin, coordinates_source_label,
                        coordinates_updated_at_ms
                 FROM photo_library_metadata_overrides
                 WHERE photo_id = ?1",
                [photo_id.as_bytes().as_slice()],
                read_metadata_overrides,
            )
            .optional()?
            .map_or_else(
                || {
                    Ok(PhotoLibraryMetadataOverrides {
                        photo_id,
                        capture_time: None,
                        coordinates: None,
                    })
                },
                Ok,
            )
    }

    pub fn effective_photo_library_facts(
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
                 FROM photo_library_effective_facts WHERE photo_id = ?1",
                [photo_id.as_bytes().as_slice()],
                read_library_facts,
            )
            .optional()
            .map_err(Into::into)
    }
}

fn set_metadata_overrides_in_transaction(
    transaction: &Transaction<'_>,
    command: &SetPhotoLibraryMetadataOverrides,
) -> Result<(), CatalogError> {
    ensure_photo_exists(transaction, command.photo_id)?;
    if matches!(
        &command.capture_time,
        LibraryMetadataOverrideAction::Unchanged
    ) && matches!(
        &command.coordinates,
        LibraryMetadataOverrideAction::Unchanged
    ) {
        return Ok(());
    }
    transaction.execute(
        "INSERT INTO photo_library_metadata_overrides(photo_id)
         VALUES (?1) ON CONFLICT(photo_id) DO NOTHING",
        [command.photo_id.as_bytes().as_slice()],
    )?;
    update_capture_time(transaction, command)?;
    update_coordinates(transaction, command)?;
    transaction.execute(
        "DELETE FROM photo_library_metadata_overrides
         WHERE photo_id = ?1
           AND capture_time_mode IS NULL
           AND coordinates_mode IS NULL",
        [command.photo_id.as_bytes().as_slice()],
    )?;
    refresh_effective_photo_library_facts(transaction, command.photo_id)
}

fn validate_command(command: &SetPhotoLibraryMetadataOverrides) -> Result<(), CatalogError> {
    if command.updated_at_ms < 0 {
        return Err(invalid("updated_at_ms must be non-negative"));
    }
    if command.source_label.chars().count() > 1_024 {
        return Err(invalid("source label exceeds 1024 characters"));
    }
    if let LibraryMetadataOverrideAction::Set(captured_at) = &command.capture_time
        && capture_day(*captured_at).is_empty()
    {
        return Err(invalid(
            "capture time is outside the supported calendar range",
        ));
    }
    if let LibraryMetadataOverrideAction::Set(coordinates) = &command.coordinates {
        validate_coordinates(coordinates)?;
    }
    Ok(())
}

fn validate_coordinates(coordinates: &LibraryCoordinates) -> Result<(), CatalogError> {
    if !(-900_000_000..=900_000_000).contains(&coordinates.latitude_e7) {
        return Err(invalid("latitude is outside -90 through 90 degrees"));
    }
    if !(-1_800_000_000..=1_800_000_000).contains(&coordinates.longitude_e7) {
        return Err(invalid("longitude is outside -180 through 180 degrees"));
    }
    if coordinates.place_name.chars().count() > 1_024 {
        return Err(invalid("place name exceeds 1024 characters"));
    }
    Ok(())
}

fn invalid(message: impl Into<String>) -> CatalogError {
    CatalogError::InvalidLibraryFacts(message.into())
}

fn update_capture_time(
    transaction: &Transaction<'_>,
    command: &SetPhotoLibraryMetadataOverrides,
) -> Result<(), CatalogError> {
    let origin = command.origin.as_str();
    match command.capture_time {
        LibraryMetadataOverrideAction::Unchanged => {}
        LibraryMetadataOverrideAction::Inherit => {
            transaction.execute(
                "UPDATE photo_library_metadata_overrides
                 SET capture_time_mode = NULL,
                     captured_at_unix_seconds = NULL,
                     capture_day = '',
                     capture_time_origin = '',
                     capture_time_source_label = '',
                     capture_time_updated_at_ms = NULL
                 WHERE photo_id = ?1",
                [command.photo_id.as_bytes().as_slice()],
            )?;
        }
        LibraryMetadataOverrideAction::Clear => {
            transaction.execute(
                "UPDATE photo_library_metadata_overrides
                 SET capture_time_mode = 'clear',
                     captured_at_unix_seconds = NULL,
                     capture_day = '',
                     capture_time_origin = ?2,
                     capture_time_source_label = ?3,
                     capture_time_updated_at_ms = ?4
                 WHERE photo_id = ?1",
                params![
                    command.photo_id.as_bytes().as_slice(),
                    origin,
                    command.source_label.trim(),
                    command.updated_at_ms,
                ],
            )?;
        }
        LibraryMetadataOverrideAction::Set(captured_at) => {
            transaction.execute(
                "UPDATE photo_library_metadata_overrides
                 SET capture_time_mode = 'set',
                     captured_at_unix_seconds = ?2,
                     capture_day = ?3,
                     capture_time_origin = ?4,
                     capture_time_source_label = ?5,
                     capture_time_updated_at_ms = ?6
                 WHERE photo_id = ?1",
                params![
                    command.photo_id.as_bytes().as_slice(),
                    captured_at,
                    capture_day(captured_at),
                    origin,
                    command.source_label.trim(),
                    command.updated_at_ms,
                ],
            )?;
        }
    }
    Ok(())
}

fn update_coordinates(
    transaction: &Transaction<'_>,
    command: &SetPhotoLibraryMetadataOverrides,
) -> Result<(), CatalogError> {
    let origin = command.origin.as_str();
    match &command.coordinates {
        LibraryMetadataOverrideAction::Unchanged => {}
        LibraryMetadataOverrideAction::Inherit => {
            transaction.execute(
                "UPDATE photo_library_metadata_overrides
                 SET coordinates_mode = NULL,
                     latitude_e7 = NULL,
                     longitude_e7 = NULL,
                     place_name = '',
                     coordinates_origin = '',
                     coordinates_source_label = '',
                     coordinates_updated_at_ms = NULL
                 WHERE photo_id = ?1",
                [command.photo_id.as_bytes().as_slice()],
            )?;
        }
        LibraryMetadataOverrideAction::Clear => {
            transaction.execute(
                "UPDATE photo_library_metadata_overrides
                 SET coordinates_mode = 'clear',
                     latitude_e7 = NULL,
                     longitude_e7 = NULL,
                     place_name = '',
                     coordinates_origin = ?2,
                     coordinates_source_label = ?3,
                     coordinates_updated_at_ms = ?4
                 WHERE photo_id = ?1",
                params![
                    command.photo_id.as_bytes().as_slice(),
                    origin,
                    command.source_label.trim(),
                    command.updated_at_ms,
                ],
            )?;
        }
        LibraryMetadataOverrideAction::Set(coordinates) => {
            transaction.execute(
                "UPDATE photo_library_metadata_overrides
                 SET coordinates_mode = 'set',
                     latitude_e7 = ?2,
                     longitude_e7 = ?3,
                     place_name = ?4,
                     coordinates_origin = ?5,
                     coordinates_source_label = ?6,
                     coordinates_updated_at_ms = ?7
                 WHERE photo_id = ?1",
                params![
                    command.photo_id.as_bytes().as_slice(),
                    coordinates.latitude_e7,
                    coordinates.longitude_e7,
                    coordinates.place_name.trim(),
                    origin,
                    command.source_label.trim(),
                    command.updated_at_ms,
                ],
            )?;
        }
    }
    Ok(())
}

pub(crate) fn refresh_effective_photo_library_facts(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    transaction.execute(
        "DELETE FROM photo_library_effective_facts
         WHERE photo_id = ?1
           AND NOT EXISTS (
               SELECT 1 FROM photo_library_facts WHERE photo_id = ?1
           )
           AND NOT EXISTS (
               SELECT 1 FROM photo_library_metadata_overrides WHERE photo_id = ?1
           )",
        [photo_id.as_bytes().as_slice()],
    )?;
    transaction.execute(
        "INSERT INTO photo_library_effective_facts(
             photo_id, captured_at_unix_seconds, capture_day,
             camera_make, camera_model, camera_key,
             lens_make, lens_model, lens_key,
             aperture_milli, focal_length_tenth_mm, iso_speed,
             latitude_e7, longitude_e7, place_name,
             indexed_representation_id, indexed_source_byte_len,
             indexed_source_modified_at_ms, indexed_at_ms
         )
         SELECT p.id,
                CASE o.capture_time_mode
                    WHEN 'set' THEN o.captured_at_unix_seconds
                    WHEN 'clear' THEN NULL
                    ELSE f.captured_at_unix_seconds
                END,
                CASE o.capture_time_mode
                    WHEN 'set' THEN o.capture_day
                    WHEN 'clear' THEN ''
                    ELSE COALESCE(f.capture_day, '')
                END,
                COALESCE(f.camera_make, ''),
                COALESCE(f.camera_model, ''),
                COALESCE(f.camera_key, ''),
                COALESCE(f.lens_make, ''),
                COALESCE(f.lens_model, ''),
                COALESCE(f.lens_key, ''),
                f.aperture_milli,
                f.focal_length_tenth_mm,
                f.iso_speed,
                CASE o.coordinates_mode
                    WHEN 'set' THEN o.latitude_e7
                    WHEN 'clear' THEN NULL
                    ELSE f.latitude_e7
                END,
                CASE o.coordinates_mode
                    WHEN 'set' THEN o.longitude_e7
                    WHEN 'clear' THEN NULL
                    ELSE f.longitude_e7
                END,
                CASE o.coordinates_mode
                    WHEN 'set' THEN o.place_name
                    WHEN 'clear' THEN ''
                    ELSE COALESCE(f.place_name, '')
                END,
                f.indexed_representation_id,
                f.indexed_source_byte_len,
                f.indexed_source_modified_at_ms,
                max(
                    COALESCE(f.indexed_at_ms, 0),
                    COALESCE(o.capture_time_updated_at_ms, 0),
                    COALESCE(o.coordinates_updated_at_ms, 0)
                )
         FROM photos p
         LEFT JOIN photo_library_facts f ON f.photo_id = p.id
         LEFT JOIN photo_library_metadata_overrides o ON o.photo_id = p.id
         WHERE p.id = ?1 AND (f.photo_id IS NOT NULL OR o.photo_id IS NOT NULL)
         ON CONFLICT(photo_id) DO UPDATE SET
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
        [photo_id.as_bytes().as_slice()],
    )?;
    Ok(())
}

fn read_metadata_overrides(row: &Row<'_>) -> rusqlite::Result<PhotoLibraryMetadataOverrides> {
    Ok(PhotoLibraryMetadataOverrides {
        photo_id: crate::row_codec::read_id(row, 0)?,
        capture_time: read_capture_time_override(row)?,
        coordinates: read_coordinates_override(row)?,
    })
}

fn read_capture_time_override(
    row: &Row<'_>,
) -> rusqlite::Result<Option<LibraryMetadataOverride<i64>>> {
    let mode: Option<String> = row.get(1)?;
    let Some(mode) = mode else {
        return Ok(None);
    };
    Ok(Some(LibraryMetadataOverride {
        value: match mode.as_str() {
            "set" => Some(row.get(2)?),
            "clear" => None,
            _ => return Err(invalid_persisted_mode("capture time", &mode)),
        },
        origin: read_origin(row.get(3)?)?,
        source_label: row.get(4)?,
        updated_at_ms: row.get(5)?,
    }))
}

fn read_coordinates_override(
    row: &Row<'_>,
) -> rusqlite::Result<Option<LibraryMetadataOverride<LibraryCoordinates>>> {
    let mode: Option<String> = row.get(6)?;
    let Some(mode) = mode else {
        return Ok(None);
    };
    Ok(Some(LibraryMetadataOverride {
        value: match mode.as_str() {
            "set" => Some(LibraryCoordinates {
                latitude_e7: row.get(7)?,
                longitude_e7: row.get(8)?,
                place_name: row.get(9)?,
            }),
            "clear" => None,
            _ => return Err(invalid_persisted_mode("coordinates", &mode)),
        },
        origin: read_origin(row.get(10)?)?,
        source_label: row.get(11)?,
        updated_at_ms: row.get(12)?,
    }))
}

fn read_origin(value: String) -> rusqlite::Result<LibraryMetadataOverrideOrigin> {
    match value.as_str() {
        "manual" => Ok(LibraryMetadataOverrideOrigin::Manual),
        "gpx" => Ok(LibraryMetadataOverrideOrigin::Gpx),
        _ => Err(rusqlite::Error::InvalidQuery),
    }
}

fn invalid_persisted_mode(label: &str, value: &str) -> rusqlite::Error {
    let _ = (label, value);
    rusqlite::Error::InvalidQuery
}

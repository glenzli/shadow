//! `SQLite` row decoding for Library projections.
//!
//! This module is the single mapping boundary between persisted columns and Library value objects.

use rusqlite::types::Type;
use shadow_domain::{AssetLocation, EntityId, ImportSessionId, PhotoId, RepresentationKind};
use uuid::Uuid;

use crate::{
    RepresentationFingerprint, SourceScanReconciliation,
    decision::photo_decision_state_from_columns, row_codec::read_id,
};

use super::{
    AlbumKind, AlbumRecord, LibraryFacetValue, LibraryPhotoFacts, LibraryPhotoRecord,
    LibrarySourceHealth, LibrarySourceRecord, MissingSourceLocationRecord, PhotoLibraryState,
};

pub(super) fn read_library_photo(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryPhotoRecord> {
    let photo_id: PhotoId = read_id(row, 0)?;
    let representation_id = read_id(row, 1)?;
    let platform = platform_from_text(&row.get::<_, String>(2)?, 2)?;
    let byte_len: i64 = row.get(5)?;
    let source = RepresentationFingerprint {
        byte_len: u64::try_from(byte_len).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(5, Type::Integer, Box::new(error))
        })?,
        modified_at_ms: row.get(6)?,
    };
    let decision = photo_decision_state_from_columns(row.get(27)?, row.get(28)?, row.get(29)?)
        .map_err(|error| invalid_data(27, error.to_string()))?;
    let representation_count: i64 = row.get(32)?;
    let source_location_count: i64 = row.get(33)?;
    Ok(LibraryPhotoRecord {
        photo_id,
        representation_id,
        representation_count: u32::try_from(representation_count).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(32, Type::Integer, Box::new(error))
        })?,
        source_location_count: u32::try_from(source_location_count).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(33, Type::Integer, Box::new(error))
        })?,
        has_raw_representation: row.get::<_, i64>(34)? != 0,
        has_raster_representation: row.get::<_, i64>(35)? != 0,
        location_id: read_id(row, 31)?,
        location: AssetLocation::new(platform, row.get(3)?, row.get::<_, String>(4)?),
        source,
        facts: read_library_facts_from_columns(row, 7)?,
        state: PhotoLibraryState {
            photo_id,
            liked: row.get::<_, i64>(24)? != 0,
            color_label: row.get(25)?,
            updated_at_ms: row.get(26)?,
        },
        decision,
        has_development_edits: row.get::<_, i64>(30)? != 0,
    })
}

pub(super) fn read_library_facet(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryFacetValue> {
    let photo_count: i64 = row.get(2)?;
    Ok(LibraryFacetValue {
        key: row.get(0)?,
        label: row.get(1)?,
        photo_count: u64::try_from(photo_count).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(2, Type::Integer, Box::new(error))
        })?,
    })
}

fn read_library_facts_from_columns(
    row: &rusqlite::Row<'_>,
    start: usize,
) -> rusqlite::Result<Option<LibraryPhotoFacts>> {
    let Some(photo_id) = optional_entity_id::<PhotoId>(row, start)? else {
        return Ok(None);
    };
    let aperture_milli: Option<i64> = row.get(start + 7)?;
    let focal_length_tenth_mm: Option<i64> = row.get(start + 8)?;
    let latitude_e7: Option<i64> = row.get(start + 10)?;
    let longitude_e7: Option<i64> = row.get(start + 11)?;
    let source_byte_len: Option<i64> = row.get(start + 14)?;
    Ok(Some(LibraryPhotoFacts {
        photo_id,
        captured_at_unix_seconds: row.get(start + 1)?,
        capture_day: row.get(start + 2)?,
        camera_make: row.get(start + 3)?,
        camera_model: row.get(start + 4)?,
        lens_make: row.get(start + 5)?,
        lens_model: row.get(start + 6)?,
        aperture_milli: optional_u32(aperture_milli, start + 7)?,
        focal_length_tenth_mm: optional_u32(focal_length_tenth_mm, start + 8)?,
        iso_speed: row.get(start + 9)?,
        latitude_e7: optional_i32(latitude_e7, start + 10)?,
        longitude_e7: optional_i32(longitude_e7, start + 11)?,
        place_name: row.get(start + 12)?,
        indexed_representation_id: optional_entity_id(row, start + 13)?,
        indexed_source: match source_byte_len {
            Some(byte_len) => Some(RepresentationFingerprint {
                byte_len: u64::try_from(byte_len).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        start + 14,
                        Type::Integer,
                        Box::new(error),
                    )
                })?,
                modified_at_ms: row.get(start + 15)?,
            }),
            None => None,
        },
        indexed_at_ms: row.get(start + 16)?,
    }))
}

pub(super) fn read_library_facts(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryPhotoFacts> {
    let aperture_milli: Option<i64> = row.get(7)?;
    let focal_length_tenth_mm: Option<i64> = row.get(8)?;
    let latitude_e7: Option<i64> = row.get(10)?;
    let longitude_e7: Option<i64> = row.get(11)?;
    let source_byte_len: Option<i64> = row.get(14)?;
    Ok(LibraryPhotoFacts {
        photo_id: read_id(row, 0)?,
        captured_at_unix_seconds: row.get(1)?,
        capture_day: row.get(2)?,
        camera_make: row.get(3)?,
        camera_model: row.get(4)?,
        lens_make: row.get(5)?,
        lens_model: row.get(6)?,
        aperture_milli: optional_u32(aperture_milli, 7)?,
        focal_length_tenth_mm: optional_u32(focal_length_tenth_mm, 8)?,
        iso_speed: row.get(9)?,
        latitude_e7: optional_i32(latitude_e7, 10)?,
        longitude_e7: optional_i32(longitude_e7, 11)?,
        place_name: row.get(12)?,
        indexed_representation_id: optional_entity_id(row, 13)?,
        indexed_source: match source_byte_len {
            Some(byte_len) => Some(RepresentationFingerprint {
                byte_len: u64::try_from(byte_len).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(14, Type::Integer, Box::new(error))
                })?,
                modified_at_ms: row.get(15)?,
            }),
            None => None,
        },
        indexed_at_ms: row.get(16)?,
    })
}

pub(super) fn read_album(row: &rusqlite::Row<'_>) -> rusqlite::Result<AlbumRecord> {
    let kind: String = row.get(1)?;
    Ok(AlbumRecord {
        id: read_id(row, 0)?,
        kind: AlbumKind::parse(&kind).map_err(|error| invalid_data(1, error.to_string()))?,
        name: row.get(2)?,
        query_json: row.get(3)?,
        created_at_ms: row.get(4)?,
        updated_at_ms: row.get(5)?,
    })
}

pub(super) fn read_library_source(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<LibrarySourceRecord> {
    let platform: String = row.get(1)?;
    let platform = platform_from_text(&platform, 1)?;
    Ok(LibrarySourceRecord {
        id: read_id(row, 0)?,
        root: AssetLocation::new(platform, row.get(2)?, row.get::<_, String>(3)?),
        enabled: row.get::<_, i64>(4)? != 0,
        created_at_ms: row.get(5)?,
        last_scanned_at_ms: row.get(6)?,
    })
}

pub(super) fn read_library_source_health(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<LibrarySourceHealth> {
    let source = read_library_source(row)?;
    let latest_session_id: Option<ImportSessionId> = optional_entity_id(row, 7)?;
    let latest_completed_scan = latest_session_id
        .map(|session_id| -> rusqlite::Result<SourceScanReconciliation> {
            let known_locations: i64 = row.get(9)?;
            let seen_locations: i64 = row.get(10)?;
            let known_locations = u64::try_from(known_locations).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(9, Type::Integer, Box::new(error))
            })?;
            let seen_locations = u64::try_from(seen_locations).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(10, Type::Integer, Box::new(error))
            })?;
            Ok(SourceScanReconciliation {
                session_id,
                source_id: source.id,
                completed_at_ms: row.get(8)?,
                known_locations,
                seen_locations,
                not_seen_locations: known_locations.saturating_sub(seen_locations),
            })
        })
        .transpose()?;
    Ok(LibrarySourceHealth {
        source,
        latest_completed_scan,
    })
}

pub(super) fn read_missing_source_location(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<MissingSourceLocationRecord> {
    let platform: String = row.get(3)?;
    let platform = platform_from_text(&platform, 3)?;
    let kind: String = row.get(6)?;
    let byte_len: i64 = row.get(7)?;
    let byte_len = u64::try_from(byte_len).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(7, Type::Integer, Box::new(error))
    })?;
    Ok(MissingSourceLocationRecord {
        photo_id: read_id(row, 0)?,
        representation_id: read_id(row, 1)?,
        location_id: read_id(row, 2)?,
        kind: representation_kind_from_text(&kind, 6)?,
        location: AssetLocation::new(platform, row.get(4)?, row.get::<_, String>(5)?),
        source: RepresentationFingerprint {
            byte_len,
            modified_at_ms: row.get(8)?,
        },
        captured_at_unix_seconds: row.get(9)?,
        camera_key: row.get::<_, Option<String>>(10)?.unwrap_or_default(),
        last_seen_at_ms: row.get(11)?,
    })
}

pub(crate) fn representation_kind_from_text(
    kind: &str,
    index: usize,
) -> rusqlite::Result<RepresentationKind> {
    match kind {
        "original_raw" => Ok(RepresentationKind::OriginalRaw),
        "original_raster" => Ok(RepresentationKind::OriginalRaster),
        "derived_dng" => Ok(RepresentationKind::DerivedDng),
        "embedded_preview" => Ok(RepresentationKind::EmbeddedPreview),
        "scene_linear_rgb" => Ok(RepresentationKind::SceneLinearRgb),
        "vendor_rendered_rgb" => Ok(RepresentationKind::VendorRenderedRgb),
        "proxy" => Ok(RepresentationKind::Proxy),
        _ => Err(invalid_data(
            index,
            format!("unknown persisted representation kind {kind:?}"),
        )),
    }
}

pub(crate) fn platform_from_text(
    platform: &str,
    index: usize,
) -> rusqlite::Result<shadow_domain::Platform> {
    match platform {
        "macos" => Ok(shadow_domain::Platform::MacOs),
        "windows" => Ok(shadow_domain::Platform::Windows),
        "other_unix" => Ok(shadow_domain::Platform::OtherUnix),
        _ => Err(invalid_data(
            index,
            format!("unknown platform {platform:?}"),
        )),
    }
}

fn optional_u32(value: Option<i64>, index: usize) -> rusqlite::Result<Option<u32>> {
    value
        .map(|value| {
            u32::try_from(value).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
            })
        })
        .transpose()
}

fn optional_entity_id<I: EntityId>(
    row: &rusqlite::Row<'_>,
    index: usize,
) -> rusqlite::Result<Option<I>> {
    let bytes: Option<Vec<u8>> = row.get(index)?;
    bytes
        .map(|bytes| {
            let uuid = Uuid::from_slice(&bytes).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
            })?;
            Ok(I::from_uuid(uuid))
        })
        .transpose()
}

fn optional_i32(value: Option<i64>, index: usize) -> rusqlite::Result<Option<i32>> {
    value
        .map(|value| {
            i32::try_from(value).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
            })
        })
        .transpose()
}

fn invalid_data(index: usize, message: String) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(
        index,
        Type::Text,
        Box::new(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            message,
        )),
    )
}

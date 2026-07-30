//! Projection of decoder metadata into the compact, photo-first Library index.
//!
//! Decoder snapshots remain the lossless provider-neutral record. This module
//! deliberately copies only the small set of fields that are useful for
//! high-cardinality Library filtering, grouping, and keyset pagination.

use rusqlite::{OptionalExtension, Transaction};
use shadow_domain::{EntityId, PhotoId, RawMetadataSnapshot, RepresentationId};

use crate::library::upsert_photo_library_facts_in_transaction;
use crate::{CatalogError, LibraryPhotoFacts, RepresentationFingerprint, row_codec::read_id};

pub(crate) fn project_decoder_metadata_into_library_facts(
    transaction: &Transaction<'_>,
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    metadata: &RawMetadataSnapshot,
    indexed_at_ms: i64,
) -> Result<(), CatalogError> {
    let photo_id: Option<PhotoId> = transaction
        .query_row(
            "SELECT photo_id FROM representations WHERE id = ?1",
            [representation_id.as_bytes().as_slice()],
            |row| read_id(row, 0),
        )
        .optional()?;
    let photo_id = photo_id.ok_or(CatalogError::RepresentationNotFound(representation_id))?;

    let camera_make = first_nonempty(&metadata.make, &metadata.normalized_make);
    let camera_model = first_nonempty(&metadata.model, &metadata.normalized_model);
    let coordinates = metadata.gps.as_ref().and_then(|gps| {
        Some((
            coordinate_e7(gps.latitude_degrees, 90.0)?,
            coordinate_e7(gps.longitude_degrees, 180.0)?,
        ))
    });
    let facts = LibraryPhotoFacts {
        photo_id,
        captured_at_unix_seconds: nonzero_i64(metadata.captured_at_unix_seconds),
        capture_day: capture_day(metadata.captured_at_unix_seconds),
        camera_make: bounded_text(camera_make, 256),
        camera_model: bounded_text(camera_model, 256),
        lens_make: bounded_text(&metadata.lens_make, 256),
        lens_model: bounded_text(&metadata.lens_model, 512),
        aperture_milli: scaled_positive_u32(metadata.aperture_f_number, 1_000.0),
        focal_length_tenth_mm: scaled_positive_u32(metadata.focal_length_mm, 10.0),
        iso_speed: positive_finite(metadata.iso_speed),
        latitude_e7: coordinates.map(|value| value.0),
        longitude_e7: coordinates.map(|value| value.1),
        place_name: String::new(),
        indexed_representation_id: Some(representation_id),
        indexed_source: Some(source),
        indexed_at_ms,
    };
    upsert_photo_library_facts_in_transaction(transaction, &facts)
}

fn first_nonempty<'a>(primary: &'a str, fallback: &'a str) -> &'a str {
    if primary.trim().is_empty() {
        fallback
    } else {
        primary
    }
}

fn bounded_text(value: &str, maximum_chars: usize) -> String {
    value.trim().chars().take(maximum_chars).collect()
}

fn nonzero_i64(value: i64) -> Option<i64> {
    (value != 0).then_some(value)
}

fn positive_finite(value: f64) -> Option<f64> {
    (value.is_finite() && value > 0.0).then_some(value)
}

#[allow(clippy::cast_possible_truncation)]
fn coordinate_e7(value: f64, absolute_limit: f64) -> Option<i32> {
    if !value.is_finite() || !(-absolute_limit..=absolute_limit).contains(&value) {
        return None;
    }
    Some((value * 10_000_000.0).round() as i32)
}

// The finite positive range check immediately before the conversion proves
// that the rounded value fits losslessly in `u32`.
#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
fn scaled_positive_u32(value: f64, multiplier: f64) -> Option<u32> {
    if !value.is_finite() || value <= 0.0 {
        return None;
    }
    let scaled = (value * multiplier).round();
    if !(1.0..=f64::from(u32::MAX)).contains(&scaled) {
        return None;
    }
    Some(scaled as u32)
}

/// Converts a normal Unix timestamp into its UTC calendar day without adding a
/// runtime date-time dependency to the catalog's hot import path.
pub(crate) fn capture_day(unix_seconds: i64) -> String {
    if unix_seconds == 0 {
        return String::new();
    }
    let days = unix_seconds.div_euclid(86_400);
    // The civil-date conversion below is deliberately limited to the ISO
    // four-digit range. Outlier/corrupt metadata stays queryable by timestamp
    // but is not allowed to pollute the date facet with a malformed key.
    if !(-719_162..=2_932_896).contains(&days) {
        return String::new();
    }
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let day_of_era = z - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let mut year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_prime = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    let month = month_prime + if month_prime < 10 { 3 } else { -9 };
    year += i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02}")
}

#[cfg(test)]
mod tests;

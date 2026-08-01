//! Coordinate-bound reverse-geocoding persistence and unresolved work discovery.
//!
//! Provider calls live outside the Catalog. This owner validates their small
//! structured result, derives stable facet identities, rejects results whose
//! exact coordinates are no longer used, and exposes a bounded unresolved
//! queue. Results are shared by coordinate rather than duplicated per photo.

use rusqlite::{OptionalExtension, params, types::Type};

use crate::{Catalog, CatalogError};

use super::{
    LibraryPlaceResolution, LibraryPlaceResolutionCandidate, RecordLibraryPlaceResolution,
    RecordLibraryPlaceResolutionStatus,
};

pub const MAX_LIBRARY_PLACE_RESOLUTION_CANDIDATES: usize = 256;

impl Catalog {
    /// Returns the most-used exact coordinate pairs that do not yet have a
    /// structured place result. Recording one result removes that pair from
    /// subsequent pages, so callers do not need a mutable offset cursor.
    pub fn library_place_resolution_candidates(
        &self,
        requested_limit: usize,
    ) -> Result<Vec<LibraryPlaceResolutionCandidate>, CatalogError> {
        let limit = requested_limit.clamp(1, MAX_LIBRARY_PLACE_RESOLUTION_CANDIDATES);
        let mut statement = self.connection.prepare(
            "SELECT f.latitude_e7, f.longitude_e7, COUNT(*)
             FROM photo_library_effective_facts f
             LEFT JOIN library_place_resolutions place
               ON place.latitude_e7 = f.latitude_e7
              AND place.longitude_e7 = f.longitude_e7
             WHERE f.latitude_e7 IS NOT NULL
               AND f.longitude_e7 IS NOT NULL
               AND place.latitude_e7 IS NULL
             GROUP BY f.latitude_e7, f.longitude_e7
             ORDER BY COUNT(*) DESC, f.latitude_e7, f.longitude_e7
             LIMIT ?1",
        )?;
        let rows = statement.query_map([i64::try_from(limit).unwrap_or(i64::MAX)], |row| {
            let count: i64 = row.get(2)?;
            Ok(LibraryPlaceResolutionCandidate {
                latitude_e7: row.get(0)?,
                longitude_e7: row.get(1)?,
                photo_count: u64::try_from(count).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(2, Type::Integer, Box::new(error))
                })?,
            })
        })?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Records one successful provider result only while its exact coordinate
    /// identity remains in the effective Library projection.
    pub fn record_library_place_resolution(
        &mut self,
        record: &RecordLibraryPlaceResolution,
    ) -> Result<RecordLibraryPlaceResolutionStatus, CatalogError> {
        let normalized = normalize_record(record)?;
        let transaction = self.connection.transaction()?;
        let coordinates_in_use = transaction.query_row(
            "SELECT EXISTS(
                 SELECT 1 FROM photo_library_effective_facts
                 WHERE latitude_e7 = ?1 AND longitude_e7 = ?2
             )",
            params![normalized.latitude_e7, normalized.longitude_e7],
            |row| row.get::<_, i64>(0),
        )? != 0;
        if !coordinates_in_use {
            return Ok(RecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed);
        }

        transaction.execute(
            "INSERT INTO library_place_resolutions(
                 latitude_e7, longitude_e7,
                 country_code, country_name, country_key,
                 administrative_area, locality, locality_key, locality_label,
                 display_name, provider_id, provider_version, locale, resolved_at_ms
             ) VALUES (
                 ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14
             )
             ON CONFLICT(latitude_e7, longitude_e7) DO UPDATE SET
                 country_code = excluded.country_code,
                 country_name = excluded.country_name,
                 country_key = excluded.country_key,
                 administrative_area = excluded.administrative_area,
                 locality = excluded.locality,
                 locality_key = excluded.locality_key,
                 locality_label = excluded.locality_label,
                 display_name = excluded.display_name,
                 provider_id = excluded.provider_id,
                 provider_version = excluded.provider_version,
                 locale = excluded.locale,
                 resolved_at_ms = excluded.resolved_at_ms",
            params![
                normalized.latitude_e7,
                normalized.longitude_e7,
                normalized.country_code,
                normalized.country_name,
                normalized.country_key,
                normalized.administrative_area,
                normalized.locality,
                normalized.locality_key,
                normalized.locality_label,
                normalized.display_name,
                normalized.provider_id,
                normalized.provider_version,
                normalized.locale,
                normalized.resolved_at_ms,
            ],
        )?;
        transaction.commit()?;
        Ok(RecordLibraryPlaceResolutionStatus::Recorded)
    }

    /// Reads the structured result for one exact coordinate pair.
    pub fn library_place_resolution(
        &self,
        latitude_e7: i32,
        longitude_e7: i32,
    ) -> Result<Option<LibraryPlaceResolution>, CatalogError> {
        validate_coordinates(latitude_e7, longitude_e7)?;
        self.connection
            .query_row(
                "SELECT latitude_e7, longitude_e7,
                        country_code, country_name, country_key,
                        administrative_area, locality, locality_key, locality_label,
                        display_name, provider_id, provider_version, locale, resolved_at_ms
                 FROM library_place_resolutions
                 WHERE latitude_e7 = ?1 AND longitude_e7 = ?2",
                params![latitude_e7, longitude_e7],
                |row| {
                    Ok(LibraryPlaceResolution {
                        latitude_e7: row.get(0)?,
                        longitude_e7: row.get(1)?,
                        country_code: row.get(2)?,
                        country_name: row.get(3)?,
                        country_key: row.get(4)?,
                        administrative_area: row.get(5)?,
                        locality: row.get(6)?,
                        locality_key: row.get(7)?,
                        locality_label: row.get(8)?,
                        display_name: row.get(9)?,
                        provider_id: row.get(10)?,
                        provider_version: row.get(11)?,
                        locale: row.get(12)?,
                        resolved_at_ms: row.get(13)?,
                    })
                },
            )
            .optional()
            .map_err(Into::into)
    }
}

fn normalize_record(
    source: &RecordLibraryPlaceResolution,
) -> Result<LibraryPlaceResolution, CatalogError> {
    validate_coordinates(source.latitude_e7, source.longitude_e7)?;
    if source.resolved_at_ms < 0 {
        return Err(invalid("resolution time must be nonnegative"));
    }

    let country_code = source.country_code.trim().to_ascii_uppercase();
    let country_name = source.country_name.trim().to_owned();
    let administrative_area = source.administrative_area.trim().to_owned();
    let locality = source.locality.trim().to_owned();
    let display_name = source.display_name.trim().to_owned();
    let provider_id = source.provider_id.trim().to_owned();
    let provider_version = source.provider_version.trim().to_owned();
    let locale = source.locale.trim().to_owned();
    if country_code.is_empty() && country_name.is_empty() {
        return Err(invalid("a country code or country name is required"));
    }
    if provider_id.is_empty() {
        return Err(invalid("provider id is required"));
    }
    for (name, value, maximum) in [
        ("country code", country_code.as_str(), 16),
        ("country name", country_name.as_str(), 256),
        ("administrative area", administrative_area.as_str(), 256),
        ("locality", locality.as_str(), 256),
        ("display name", display_name.as_str(), 1_024),
        ("provider id", provider_id.as_str(), 128),
        ("provider version", provider_version.as_str(), 128),
        ("locale", locale.as_str(), 64),
    ] {
        if value.len() > maximum {
            return Err(invalid(&format!(
                "{name} exceeds its {maximum}-byte persisted bound"
            )));
        }
    }

    let country_key = if country_code.is_empty() {
        normalize_key_component(&country_name)
    } else {
        country_code.to_ascii_lowercase()
    };
    if country_key.is_empty() || country_key.len() > 512 {
        return Err(invalid("derived country key is empty or too long"));
    }
    let administrative_key = normalize_key_component(&administrative_area);
    let locality_component = normalize_key_component(&locality);
    let locality_key = if locality.is_empty() {
        String::new()
    } else {
        [
            country_key.as_str(),
            administrative_key.as_str(),
            locality_component.as_str(),
        ]
        .into_iter()
        .filter(|component| !component.is_empty())
        .collect::<Vec<_>>()
        .join("\u{001f}")
    };
    if locality_key.len() > 512 {
        return Err(invalid("derived locality key is too long"));
    }
    let locality_label = locality_label(
        &locality,
        &administrative_area,
        if country_name.is_empty() {
            &country_code
        } else {
            &country_name
        },
    );
    if locality_label.len() > 1_024 {
        return Err(invalid("derived locality label is too long"));
    }

    Ok(LibraryPlaceResolution {
        latitude_e7: source.latitude_e7,
        longitude_e7: source.longitude_e7,
        country_code,
        country_name,
        country_key,
        administrative_area,
        locality,
        locality_key,
        locality_label,
        display_name,
        provider_id,
        provider_version,
        locale,
        resolved_at_ms: source.resolved_at_ms,
    })
}

fn validate_coordinates(latitude_e7: i32, longitude_e7: i32) -> Result<(), CatalogError> {
    if !(-900_000_000..=900_000_000).contains(&latitude_e7)
        || !(-1_800_000_000..=1_800_000_000).contains(&longitude_e7)
    {
        return Err(invalid(
            "coordinates are outside valid latitude/longitude bounds",
        ));
    }
    Ok(())
}

fn normalize_key_component(value: &str) -> String {
    value
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .to_lowercase()
}

fn locality_label(locality: &str, administrative_area: &str, country: &str) -> String {
    let mut parts = vec![locality];
    if !administrative_area.is_empty() && !administrative_area.eq_ignore_ascii_case(locality) {
        parts.push(administrative_area);
    }
    if !country.is_empty()
        && !country.eq_ignore_ascii_case(locality)
        && !country.eq_ignore_ascii_case(administrative_area)
    {
        parts.push(country);
    }
    parts.join(" · ")
}

fn invalid(message: &str) -> CatalogError {
    CatalogError::InvalidLibraryPlaceResolution(message.to_owned())
}

#[cfg(test)]
mod tests;

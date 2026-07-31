//! Desktop-facing metadata state and single-photo correction contracts.
//!
//! Batch capture-time and GPX operations have independent preview/apply
//! lifecycles and live in responsibility-named child owners.

mod capture_time_batch;
mod gpx;

pub(in crate::library_service) use capture_time_batch::CaptureTimePreviewRegistry;
pub(in crate::library_service) use gpx::GpxPreviewRegistry;

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    LibraryCoordinates, LibraryMetadataOverrideAction, LibraryMetadataOverrideOrigin,
    LibraryPhotoFacts, PhotoLibraryMetadataOverrides, SetPhotoLibraryMetadataOverrides,
};
use shadow_domain::PhotoId;

use crate::ffi;

use super::LibraryService;

impl LibraryService {
    pub(crate) fn metadata_state(&self, photo_id: &str) -> AnyResult<ffi::FfiLibraryMetadataState> {
        let photo_id = parse_photo_id(photo_id)?;
        let observed = self.catalog.photo_library_facts(photo_id)?;
        let effective = self.catalog.effective_photo_library_facts(photo_id)?;
        let overrides = self.catalog.photo_library_metadata_overrides(photo_id)?;
        Ok(ffi_metadata_state(
            photo_id,
            observed.as_ref(),
            effective.as_ref(),
            &overrides,
        ))
    }

    pub(crate) fn set_capture_time_override(
        &self,
        photo_id: &str,
        mode: &str,
        captured_at_unix_seconds: i64,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryMetadataState> {
        let photo_id = parse_photo_id(photo_id)?;
        let capture_time = match mode {
            "inherit" => LibraryMetadataOverrideAction::Inherit,
            "clear" => LibraryMetadataOverrideAction::Clear,
            "set" => LibraryMetadataOverrideAction::Set(captured_at_unix_seconds),
            _ => bail!("capture time override mode must be inherit, set, or clear"),
        };
        self.catalog
            .set_photo_library_metadata_overrides(&SetPhotoLibraryMetadataOverrides {
                photo_id,
                capture_time,
                coordinates: LibraryMetadataOverrideAction::Unchanged,
                origin: LibraryMetadataOverrideOrigin::Manual,
                source_label: String::new(),
                updated_at_ms: now_ms,
            })?;
        self.metadata_state(&photo_id.to_string())
    }

    pub(crate) fn set_coordinates_override(
        &self,
        photo_id: &str,
        mode: &str,
        latitude_degrees: f64,
        longitude_degrees: f64,
        place_name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryMetadataState> {
        let photo_id = parse_photo_id(photo_id)?;
        let coordinates = match mode {
            "inherit" => LibraryMetadataOverrideAction::Inherit,
            "clear" => LibraryMetadataOverrideAction::Clear,
            "set" => LibraryMetadataOverrideAction::Set(LibraryCoordinates {
                latitude_e7: coordinate_e7(latitude_degrees, 90.0, "latitude")?,
                longitude_e7: coordinate_e7(longitude_degrees, 180.0, "longitude")?,
                place_name: place_name.into(),
            }),
            _ => bail!("coordinates override mode must be inherit, set, or clear"),
        };
        self.catalog
            .set_photo_library_metadata_overrides(&SetPhotoLibraryMetadataOverrides {
                photo_id,
                capture_time: LibraryMetadataOverrideAction::Unchanged,
                coordinates,
                origin: LibraryMetadataOverrideOrigin::Manual,
                source_label: String::new(),
                updated_at_ms: now_ms,
            })?;
        self.metadata_state(&photo_id.to_string())
    }
}

fn ffi_metadata_state(
    photo_id: PhotoId,
    observed: Option<&LibraryPhotoFacts>,
    effective: Option<&LibraryPhotoFacts>,
    overrides: &PhotoLibraryMetadataOverrides,
) -> ffi::FfiLibraryMetadataState {
    let observed_capture = observed.and_then(|facts| facts.captured_at_unix_seconds);
    let effective_capture = effective.and_then(|facts| facts.captured_at_unix_seconds);
    let observed_coordinates = observed.and_then(coordinates_from_facts);
    let effective_coordinates = effective.and_then(coordinates_from_facts);
    let (capture_mode, capture_origin, capture_source) =
        override_identity(overrides.capture_time.as_ref());
    let (coordinates_mode, coordinates_origin, coordinates_source) =
        override_identity(overrides.coordinates.as_ref());
    ffi::FfiLibraryMetadataState {
        photo_id: photo_id.to_string(),
        has_observed_capture_time: observed_capture.is_some(),
        observed_captured_at_unix_seconds: observed_capture.unwrap_or_default(),
        has_effective_capture_time: effective_capture.is_some(),
        effective_captured_at_unix_seconds: effective_capture.unwrap_or_default(),
        capture_time_override_mode: capture_mode,
        capture_time_override_origin: capture_origin,
        capture_time_source_label: capture_source,
        has_observed_coordinates: observed_coordinates.is_some(),
        observed_latitude_e7: observed_coordinates
            .as_ref()
            .map_or(0, |coordinates| coordinates.latitude_e7),
        observed_longitude_e7: observed_coordinates
            .as_ref()
            .map_or(0, |coordinates| coordinates.longitude_e7),
        has_effective_coordinates: effective_coordinates.is_some(),
        effective_latitude_e7: effective_coordinates
            .as_ref()
            .map_or(0, |coordinates| coordinates.latitude_e7),
        effective_longitude_e7: effective_coordinates
            .as_ref()
            .map_or(0, |coordinates| coordinates.longitude_e7),
        effective_place_name: effective_coordinates
            .map_or_else(String::new, |coordinates| coordinates.place_name),
        coordinates_override_mode: coordinates_mode,
        coordinates_override_origin: coordinates_origin,
        coordinates_source_label: coordinates_source,
    }
}

fn coordinates_from_facts(facts: &LibraryPhotoFacts) -> Option<LibraryCoordinates> {
    Some(LibraryCoordinates {
        latitude_e7: facts.latitude_e7?,
        longitude_e7: facts.longitude_e7?,
        place_name: facts.place_name.clone(),
    })
}

fn override_identity<T>(
    value: Option<&shadow_catalog::LibraryMetadataOverride<T>>,
) -> (String, String, String) {
    let Some(value) = value else {
        return ("inherit".into(), String::new(), String::new());
    };
    (
        if value.value.is_some() {
            "set"
        } else {
            "clear"
        }
        .into(),
        value.origin.as_str().into(),
        value.source_label.clone(),
    )
}

pub(super) fn parse_photo_id(value: &str) -> AnyResult<PhotoId> {
    value.parse().context("parse Library photo id")
}

#[allow(clippy::cast_possible_truncation)]
fn coordinate_e7(value: f64, limit: f64, label: &str) -> AnyResult<i32> {
    if !value.is_finite() || !(-limit..=limit).contains(&value) {
        bail!("{label} is outside its valid range");
    }
    Ok((value * 10_000_000.0).round() as i32)
}

pub(super) fn u32_count(value: usize) -> AnyResult<u32> {
    u32::try_from(value).context("Library metadata count exceeds u32")
}

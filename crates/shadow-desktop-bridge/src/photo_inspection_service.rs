//! Exact selected-photo inspection for the desktop.
//!
//! This service is intentionally independent from virtualized Library pages
//! and Review visual handles. Its only identity is the requested
//! `{photo_id, representation_id}` pair.

use anyhow::{Context, Result as AnyResult};
use shadow_catalog::{CatalogHandle, PhotoInspectionRecord, TechnicalObservationRevision};
use shadow_core::technical_analysis_preprocessing_version;
use shadow_domain::{FocusObservationSource, PhotoId, RepresentationId};

use crate::ffi;

#[derive(Debug)]
pub(crate) struct PhotoInspectionService {
    catalog: CatalogHandle,
}

impl PhotoInspectionService {
    pub(crate) const fn new(catalog: CatalogHandle) -> Self {
        Self { catalog }
    }

    pub(crate) fn inspect(
        &self,
        photo_id: &str,
        representation_id: &str,
    ) -> AnyResult<ffi::FfiPhotoInspection> {
        let photo_id = photo_id
            .parse::<PhotoId>()
            .context("parse selected photo id")?;
        let representation_id = representation_id
            .parse::<RepresentationId>()
            .context("parse selected representation id")?;
        let revision =
            TechnicalObservationRevision::current(technical_analysis_preprocessing_version());
        let record = self
            .catalog
            .photo_inspection(photo_id, representation_id, &revision)?;
        let Some(record) = record else {
            return Ok(unavailable(photo_id, representation_id));
        };
        let effective_facts = self.catalog.effective_photo_library_facts(photo_id)?;
        Ok(inspection(record, effective_facts.as_ref()))
    }
}

fn unavailable(photo_id: PhotoId, representation_id: RepresentationId) -> ffi::FfiPhotoInspection {
    ffi::FfiPhotoInspection {
        available: false,
        photo_id: photo_id.to_string(),
        representation_id: representation_id.to_string(),
        source_path: String::new(),
        source_byte_len: 0,
        has_source_modified_at: false,
        source_modified_at_ms: 0,
        has_metadata: false,
        camera_make: String::new(),
        camera_model: String::new(),
        lens_make: String::new(),
        lens_model: String::new(),
        has_captured_at: false,
        captured_at_unix_seconds: 0,
        has_coordinates: false,
        latitude_e7: 0,
        longitude_e7: 0,
        place_name: String::new(),
        has_iso_speed: false,
        iso_speed: 0.0,
        has_exposure_time: false,
        exposure_time_seconds: 0.0,
        has_aperture: false,
        aperture_f_number: 0.0,
        has_focal_length: false,
        focal_length_mm: 0.0,
        has_focal_length_35mm: false,
        focal_length_35mm: 0.0,
        has_raw_dimensions: false,
        raw_width: 0,
        raw_height: 0,
        has_sensor_bits: false,
        sensor_bits: 0,
        cfa_pattern: String::new(),
        dng_version: String::new(),
        has_focus_observation: false,
        focus_observation_schema_version: 0,
        focus_observation_source: String::new(),
        focus_observation_center_x: 0.0,
        focus_observation_center_y: 0.0,
        focus_observation_width: 0.0,
        focus_observation_height: 0.0,
        focus_observation_confirmed: false,
        focus_observation_confidence: 0.0,
        has_technical_observation: false,
        technical_input_width: 0,
        technical_input_height: 0,
        technical_preprocessing_version: String::new(),
        technical_implementation_version: String::new(),
        mean_luma: 0.0,
        p01_luma: 0.0,
        p50_luma: 0.0,
        p99_luma: 0.0,
        near_black_fraction: 0.0,
        near_white_fraction: 0.0,
        laplacian_variance: 0.0,
        edge_energy: 0.0,
    }
}

#[allow(clippy::too_many_lines)]
fn inspection(
    record: PhotoInspectionRecord,
    effective_facts: Option<&shadow_catalog::LibraryPhotoFacts>,
) -> ffi::FfiPhotoInspection {
    let has_source_modified_at = record.source.modified_at_ms.is_some();
    let source_modified_at_ms = record.source.modified_at_ms.unwrap_or_default();
    let metadata = record.metadata;
    let has_metadata = metadata.is_some();
    let focus_observation = metadata
        .as_ref()
        .and_then(|metadata| metadata.focus_observation.as_ref())
        .filter(|observation| observation.is_valid())
        .cloned();
    let (
        camera_make,
        camera_model,
        lens_make,
        lens_model,
        captured_at_unix_seconds,
        iso_speed,
        exposure_time_seconds,
        aperture_f_number,
        focal_length_mm,
        focal_length_35mm,
        raw_width,
        raw_height,
        sensor_bits,
        cfa_pattern,
        dng_version,
    ) = metadata.map_or_else(
        || {
            (
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                0,
                0.0,
                0.0,
                0.0,
                0.0,
                0.0,
                0,
                0,
                0,
                String::new(),
                String::new(),
            )
        },
        |metadata| {
            (
                metadata.make,
                metadata.model,
                metadata.lens_make,
                metadata.lens_model,
                metadata.captured_at_unix_seconds,
                metadata.iso_speed,
                metadata.exposure_time_seconds,
                metadata.aperture_f_number,
                metadata.focal_length_mm,
                metadata.focal_length_35mm,
                metadata.raw_dimensions.width,
                metadata.raw_dimensions.height,
                metadata.sensor_bits,
                metadata.cfa_pattern,
                metadata.dng_version.unwrap_or_default(),
            )
        },
    );
    let effective_captured_at = effective_facts
        .and_then(|facts| facts.captured_at_unix_seconds)
        .or_else(|| (captured_at_unix_seconds != 0).then_some(captured_at_unix_seconds));
    let effective_coordinates = effective_facts.and_then(|facts| {
        Some((
            facts.latitude_e7?,
            facts.longitude_e7?,
            facts.place_name.clone(),
        ))
    });
    let focus_observation_source =
        focus_observation
            .as_ref()
            .map_or("", |observation| match observation.source {
                FocusObservationSource::Unknown => "",
                FocusObservationSource::CameraFocusArea => "camera_focus_area",
                FocusObservationSource::CameraFocusLocation => "camera_focus_location",
            });
    let technical = record.technical;
    let has_technical_observation = technical.is_some();
    let (
        technical_input_width,
        technical_input_height,
        technical_preprocessing_version,
        technical_implementation_version,
        mean_luma,
        p01_luma,
        p50_luma,
        p99_luma,
        near_black_fraction,
        near_white_fraction,
        laplacian_variance,
        edge_energy,
    ) = technical.map_or_else(
        || {
            (
                0,
                0,
                String::new(),
                String::new(),
                0.0,
                0.0,
                0.0,
                0.0,
                0.0,
                0.0,
                0.0,
                0.0,
            )
        },
        |technical| {
            (
                technical.input_width,
                technical.input_height,
                technical.preprocessing_version,
                technical.implementation_version,
                technical.mean_luma,
                technical.p01_luma,
                technical.p50_luma,
                technical.p99_luma,
                technical.near_black_fraction,
                technical.near_white_fraction,
                technical.laplacian_variance,
                technical.edge_energy,
            )
        },
    );
    ffi::FfiPhotoInspection {
        available: true,
        photo_id: record.photo_id.to_string(),
        representation_id: record.representation_id.to_string(),
        source_path: record.location.display_path,
        source_byte_len: record.source.byte_len,
        has_source_modified_at,
        source_modified_at_ms,
        has_metadata,
        camera_make,
        camera_model,
        lens_make,
        lens_model,
        has_captured_at: effective_captured_at.is_some(),
        captured_at_unix_seconds: effective_captured_at.unwrap_or_default(),
        has_coordinates: effective_coordinates.is_some(),
        latitude_e7: effective_coordinates
            .as_ref()
            .map_or(0, |coordinates| coordinates.0),
        longitude_e7: effective_coordinates
            .as_ref()
            .map_or(0, |coordinates| coordinates.1),
        place_name: effective_coordinates.map_or_else(String::new, |coordinates| coordinates.2),
        has_iso_speed: has_metadata && iso_speed > 0.0,
        iso_speed,
        has_exposure_time: has_metadata && exposure_time_seconds > 0.0,
        exposure_time_seconds,
        has_aperture: has_metadata && aperture_f_number > 0.0,
        aperture_f_number,
        has_focal_length: has_metadata && focal_length_mm > 0.0,
        focal_length_mm,
        has_focal_length_35mm: has_metadata && focal_length_35mm > 0.0,
        focal_length_35mm,
        has_raw_dimensions: has_metadata && raw_width > 0 && raw_height > 0,
        raw_width,
        raw_height,
        has_sensor_bits: has_metadata && sensor_bits > 0,
        sensor_bits,
        cfa_pattern,
        dng_version,
        has_focus_observation: focus_observation.is_some(),
        focus_observation_schema_version: focus_observation
            .as_ref()
            .map_or(0, |observation| observation.schema_version),
        focus_observation_source: focus_observation_source.to_owned(),
        focus_observation_center_x: focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.center_x),
        focus_observation_center_y: focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.center_y),
        focus_observation_width: focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.width),
        focus_observation_height: focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.height),
        focus_observation_confirmed: focus_observation
            .as_ref()
            .is_some_and(|observation| observation.focus_confirmed),
        focus_observation_confidence: focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.confidence),
        has_technical_observation,
        technical_input_width,
        technical_input_height,
        technical_preprocessing_version,
        technical_implementation_version,
        mean_luma,
        p01_luma,
        p50_luma,
        p99_luma,
        near_black_fraction,
        near_white_fraction,
        laplacian_variance,
        edge_energy,
    }
}

#[cfg(test)]
mod tests;

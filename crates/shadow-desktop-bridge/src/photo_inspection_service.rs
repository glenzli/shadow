//! Exact selected-photo inspection for the desktop.
//!
//! This service is intentionally independent from virtualized Library pages
//! and Review visual handles. Its only identity is the requested
//! `{photo_id, representation_id}` pair.

use anyhow::{Context, Result as AnyResult};
use shadow_catalog::{CatalogHandle, PhotoInspectionRecord, TechnicalObservationRevision};
use shadow_core::technical_analysis_preprocessing_version;
use shadow_domain::{PhotoId, RepresentationId};

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
        Ok(record.map_or_else(|| unavailable(photo_id, representation_id), inspection))
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
fn inspection(record: PhotoInspectionRecord) -> ffi::FfiPhotoInspection {
    let has_source_modified_at = record.source.modified_at_ms.is_some();
    let source_modified_at_ms = record.source.modified_at_ms.unwrap_or_default();
    let metadata = record.metadata;
    let has_metadata = metadata.is_some();
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
        has_captured_at: has_metadata && captured_at_unix_seconds != 0,
        captured_at_unix_seconds,
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

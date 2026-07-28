//! Review page projection and cursor ownership.
//!
//! This module maps durable Catalog records into the desktop ABI. Visual
//! authorization remains owned by the sibling handle lifecycle.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, bail};
use shadow_bridge::{edit_preview_generator_implementation_identity, photo_provider_version};
use shadow_catalog::{
    CachedArtifactGeneratorIdentity, ReviewCursor, ReviewItemRecord, TechnicalObservationRevision,
};
use shadow_core::technical_analysis_preprocessing_version;

use crate::{
    ffi,
    preview_cache_identity::{
        EDIT_PREVIEW_GENERATOR_ID, current_source_environment_cache_identity,
        edit_preview_generator_version,
    },
};

use super::{ReviewService, ffi_decision_flag};

impl ReviewService {
    pub(crate) fn review_page(
        &self,
        cursor_path: &str,
        cursor_representation_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiReviewPage> {
        let cursor = parse_cursor(cursor_path, cursor_representation_id)?;
        let revision =
            TechnicalObservationRevision::current(technical_analysis_preprocessing_version());
        let source_environment =
            current_source_environment_cache_identity(&photo_provider_version());
        let recipe_preview_generator = CachedArtifactGeneratorIdentity {
            generator_id: EDIT_PREVIEW_GENERATOR_ID.to_owned(),
            generator_version: edit_preview_generator_version(
                &source_environment,
                &edit_preview_generator_implementation_identity(),
            ),
        };
        let page = self
            .catalog
            .review_page_with_technical_and_recipe_preview_generator(
                cursor.as_ref(),
                usize::try_from(limit).unwrap_or(usize::MAX),
                &revision,
                &recipe_preview_generator,
            )?;
        let (has_more, next_cursor_path, next_cursor_representation_id) =
            if let Some(cursor) = page.next_cursor {
                (
                    true,
                    cursor.display_path,
                    cursor.representation_id.to_string(),
                )
            } else {
                (false, String::new(), String::new())
            };
        Ok(ffi::FfiReviewPage {
            total_items: page.total_items,
            items: page
                .items
                .into_iter()
                .map(|record| self.review_item(record))
                .collect::<AnyResult<Vec<_>>>()?,
            has_more,
            next_cursor_path,
            next_cursor_representation_id,
        })
    }

    // Keep the complete gallery DTO mapping together. Adding one EXIF field
    // then touches precisely this owner and the CXX ABI.
    #[allow(clippy::too_many_lines)]
    fn review_item(&self, record: ReviewItemRecord) -> AnyResult<ffi::FfiReviewItem> {
        let (visual_handle, visual_role, visual_width, visual_height, has_visual) = self
            .grid_visual(
                record.photo_id,
                record.representation_id,
                record.source,
                record.visual.as_ref(),
            )?
            .map_or_else(
                || (String::new(), String::new(), 0, 0, false),
                |visual| {
                    (
                        visual.handle,
                        visual.role,
                        visual.dimensions.width,
                        visual.dimensions.height,
                        true,
                    )
                },
            );
        let technical = record.technical;
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
        Ok(ffi::FfiReviewItem {
            photo_id: record.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            visual_handle,
            decision_head_sequence: record.decision.head_sequence,
            decision_flag: ffi_decision_flag(record.decision.flag),
            decision_rating: record.decision.rating,
            has_development_edits: record.has_development_edits,
            title: file_name(&record.location.display_path),
            source_path: record.location.display_path,
            visual_role,
            visual_width,
            visual_height,
            has_visual,
            has_metadata,
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
        })
    }
}

pub(crate) fn parse_cursor(path: &str, representation_id: &str) -> AnyResult<Option<ReviewCursor>> {
    match (path.is_empty(), representation_id.is_empty()) {
        (true, true) => Ok(None),
        (false, false) => Ok(Some(ReviewCursor {
            display_path: path.to_owned(),
            representation_id: representation_id
                .parse()
                .with_context(|| format!("parse Review cursor id {representation_id}"))?,
        })),
        _ => bail!("Review cursor path and representation id must both be present"),
    }
}

pub(crate) fn file_name(display_path: &str) -> String {
    Path::new(display_path)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or(display_path)
        .to_owned()
}

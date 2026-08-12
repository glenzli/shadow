//! Previewed assignment of one coordinate to a selected photo set.
//!
//! Search and map providers only propose WGS84 coordinates. This owner decides
//! which logical photos may be mutated, records the conflict policy in an
//! opaque one-time preview, and applies the complete Catalog batch atomically.

use std::collections::{HashSet, VecDeque};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    LibraryCoordinates, LibraryMetadataOverrideAction, LibraryMetadataOverrideOrigin,
    SetPhotoLibraryMetadataOverrides,
};
use shadow_domain::PhotoId;
use uuid::Uuid;

use crate::{ffi, library_service::LibraryService};

use super::{coordinate_e7, parse_photo_id, u32_count};

const MAX_SELECTION: usize = 100_000;
const MAX_PREVIEWS: usize = 8;

#[derive(Debug)]
struct StoredCoordinateBatchPreview {
    mode: &'static str,
    coordinates: LibraryCoordinates,
    source_label: String,
    requested_photo_count: usize,
    missing_photo_count: usize,
    existing_photo_count: usize,
    replacement_photo_count: usize,
    photo_ids: Vec<PhotoId>,
}

#[derive(Debug, Default)]
pub(in crate::library_service) struct CoordinateBatchPreviewRegistry {
    entries: VecDeque<(String, StoredCoordinateBatchPreview)>,
}

impl CoordinateBatchPreviewRegistry {
    fn insert(&mut self, preview: StoredCoordinateBatchPreview) -> String {
        while self.entries.len() >= MAX_PREVIEWS {
            self.entries.pop_front();
        }
        let id = Uuid::now_v7().to_string();
        self.entries.push_back((id.clone(), preview));
        id
    }

    fn take(&mut self, id: &str) -> Option<StoredCoordinateBatchPreview> {
        let index = self
            .entries
            .iter()
            .position(|(candidate, _)| candidate == id)?;
        self.entries.remove(index).map(|(_, preview)| preview)
    }
}

impl LibraryService {
    pub(crate) fn preview_coordinate_batch(
        &self,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
        mode: &str,
        latitude_degrees: f64,
        longitude_degrees: f64,
        place_name: &str,
        source_label: &str,
    ) -> AnyResult<ffi::FfiCoordinateBatchPreview> {
        if targets.len() > MAX_SELECTION {
            bail!("coordinate selection exceeds {MAX_SELECTION} photos");
        }
        let mode = match mode {
            "missing" => "missing",
            "replace" => "replace",
            _ => bail!("coordinate batch mode must be missing or replace"),
        };
        let coordinates = LibraryCoordinates {
            latitude_e7: coordinate_e7(latitude_degrees, 90.0, "latitude")?,
            longitude_e7: coordinate_e7(longitude_degrees, 180.0, "longitude")?,
            place_name: place_name.trim().into(),
        };
        if coordinates.place_name.chars().count() > 1_024 {
            bail!("place name exceeds 1024 characters");
        }
        if source_label.trim().chars().count() > 1_024 {
            bail!("coordinate source label exceeds 1024 characters");
        }
        let mut seen = HashSet::with_capacity(targets.len());
        let mut photo_ids = Vec::with_capacity(targets.len());
        let mut missing_photo_count = 0;
        let mut existing_photo_count = 0;
        for target in targets {
            let photo_id = parse_photo_id(&target.photo_id)?;
            if !seen.insert(photo_id) {
                continue;
            }
            let has_coordinates = self
                .catalog
                .effective_photo_library_facts(photo_id)?
                .is_some_and(|facts| facts.latitude_e7.is_some() && facts.longitude_e7.is_some());
            if has_coordinates {
                existing_photo_count += 1;
            } else {
                missing_photo_count += 1;
            }
            if mode == "replace" || !has_coordinates {
                photo_ids.push(photo_id);
            }
        }
        let replacement_photo_count = if mode == "replace" {
            existing_photo_count
        } else {
            0
        };
        let stored = StoredCoordinateBatchPreview {
            mode,
            coordinates,
            source_label: source_label.trim().into(),
            requested_photo_count: seen.len(),
            missing_photo_count,
            existing_photo_count,
            replacement_photo_count,
            photo_ids,
        };
        let mut registry = self
            .coordinate_batch_previews
            .lock()
            .map_err(|_| anyhow!("coordinate preview registry is unavailable"))?;
        let preview_id = registry.insert(stored);
        let preview = registry
            .entries
            .back()
            .map(|(_, value)| value)
            .expect("inserted preview remains present");
        ffi_preview(preview_id, preview)
    }

    pub(crate) fn apply_coordinate_batch(
        &self,
        preview_id: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryMetadataBatchReceipt> {
        let stored = self
            .coordinate_batch_previews
            .lock()
            .map_err(|_| anyhow!("coordinate preview registry is unavailable"))?
            .take(preview_id)
            .context("coordinate preview expired or was already applied")?;
        let commands = stored
            .photo_ids
            .iter()
            .map(|photo_id| SetPhotoLibraryMetadataOverrides {
                photo_id: *photo_id,
                capture_time: LibraryMetadataOverrideAction::Unchanged,
                coordinates: LibraryMetadataOverrideAction::Set(stored.coordinates.clone()),
                origin: LibraryMetadataOverrideOrigin::Manual,
                source_label: stored.source_label.clone(),
                updated_at_ms: now_ms,
            })
            .collect::<Vec<_>>();
        self.catalog
            .set_photo_library_metadata_overrides_batch(&commands)?;
        Ok(ffi::FfiLibraryMetadataBatchReceipt {
            requested_photo_count: u32_count(stored.requested_photo_count)?,
            applied_photo_count: u32_count(commands.len())?,
        })
    }
}

fn ffi_preview(
    preview_id: String,
    preview: &StoredCoordinateBatchPreview,
) -> AnyResult<ffi::FfiCoordinateBatchPreview> {
    Ok(ffi::FfiCoordinateBatchPreview {
        preview_id,
        mode: preview.mode.into(),
        latitude_e7: preview.coordinates.latitude_e7,
        longitude_e7: preview.coordinates.longitude_e7,
        place_name: preview.coordinates.place_name.clone(),
        requested_photo_count: u32_count(preview.requested_photo_count)?,
        applicable_photo_count: u32_count(preview.photo_ids.len())?,
        skipped_photo_count: u32_count(
            preview
                .requested_photo_count
                .saturating_sub(preview.photo_ids.len()),
        )?,
        missing_photo_count: u32_count(preview.missing_photo_count)?,
        existing_photo_count: u32_count(preview.existing_photo_count)?,
        replacement_photo_count: u32_count(preview.replacement_photo_count)?,
    })
}

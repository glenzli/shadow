//! Session-bound GPX preview and one-time atomic application.

use std::{
    collections::{HashSet, VecDeque},
    path::Path,
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    LibraryMetadataOverrideAction, LibraryMetadataOverrideOrigin, SetPhotoLibraryMetadataOverrides,
};
use shadow_core::{
    GpsMatchPreview, GpsMatchSettings, GpsPhotoCapture, load_gpx_track, match_photos_to_gpx,
};
use uuid::Uuid;

use crate::{digest_hex::encode_hex, ffi};

use super::{parse_photo_id, u32_count};
use crate::library_service::LibraryService;

const MAX_GPX_SELECTION: usize = 100_000;
const MAX_GPX_PREVIEWS: usize = 8;
const MAX_GPX_PROPOSAL_SAMPLE: usize = 200;

#[derive(Debug)]
struct StoredGpxPreview {
    preview: GpsMatchPreview,
}

#[derive(Debug, Default)]
pub(in crate::library_service) struct GpxPreviewRegistry {
    entries: VecDeque<(String, StoredGpxPreview)>,
}

impl GpxPreviewRegistry {
    fn insert(&mut self, preview: GpsMatchPreview) -> String {
        while self.entries.len() >= MAX_GPX_PREVIEWS {
            self.entries.pop_front();
        }
        let id = Uuid::now_v7().to_string();
        self.entries
            .push_back((id.clone(), StoredGpxPreview { preview }));
        id
    }

    fn take(&mut self, id: &str) -> Option<StoredGpxPreview> {
        let index = self
            .entries
            .iter()
            .position(|(candidate, _)| candidate == id)?;
        self.entries.remove(index).map(|(_, preview)| preview)
    }
}

impl LibraryService {
    #[cfg(test)]
    pub(crate) fn preview_gpx_import(
        &self,
        gpx_path: &str,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
        settings: GpsMatchSettings,
    ) -> AnyResult<ffi::FfiGpxImportPreview> {
        self.preview_gpx_import_at(Path::new(gpx_path), targets, settings)
    }

    pub(crate) fn preview_gpx_import_at(
        &self,
        gpx_path: &Path,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
        settings: GpsMatchSettings,
    ) -> AnyResult<ffi::FfiGpxImportPreview> {
        if targets.len() > MAX_GPX_SELECTION {
            bail!("GPX import selection exceeds {MAX_GPX_SELECTION} photos");
        }
        let mut seen = HashSet::with_capacity(targets.len());
        let mut captures = Vec::with_capacity(targets.len());
        for target in targets {
            let photo_id = parse_photo_id(&target.photo_id)?;
            if !seen.insert(photo_id) {
                continue;
            }
            if let Some(captured_at) = self
                .catalog
                .effective_photo_library_facts(photo_id)?
                .and_then(|facts| facts.captured_at_unix_seconds)
            {
                captures.push(GpsPhotoCapture {
                    photo_id,
                    captured_at_unix_seconds: captured_at,
                });
            }
        }
        let track = load_gpx_track(gpx_path)?;
        let mut preview = match_photos_to_gpx(&track, &captures, settings)?;
        preview.requested_photo_count = seen.len();
        preview.unmatched_photo_count = seen.len().saturating_sub(preview.proposals.len());
        let ffi_preview = {
            let mut registry = self
                .gpx_previews
                .lock()
                .map_err(|_| anyhow!("GPX preview registry is unavailable"))?;
            let preview_id = registry.insert(preview.clone());
            ffi_gpx_preview(preview_id, &preview)?
        };
        Ok(ffi_preview)
    }

    pub(crate) fn apply_gpx_import(
        &self,
        preview_id: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryMetadataBatchReceipt> {
        let stored = self
            .gpx_previews
            .lock()
            .map_err(|_| anyhow!("GPX preview registry is unavailable"))?
            .take(preview_id)
            .context("GPX preview expired or was already applied")?;
        let source_label = gpx_source_label(&stored.preview);
        let commands = stored
            .preview
            .proposals
            .iter()
            .map(|proposal| SetPhotoLibraryMetadataOverrides {
                photo_id: proposal.photo_id,
                capture_time: LibraryMetadataOverrideAction::Unchanged,
                coordinates: LibraryMetadataOverrideAction::Set(proposal.coordinates.clone()),
                origin: LibraryMetadataOverrideOrigin::Gpx,
                source_label: source_label.clone(),
                updated_at_ms: now_ms,
            })
            .collect::<Vec<_>>();
        self.catalog
            .set_photo_library_metadata_overrides_batch(&commands)?;
        Ok(ffi::FfiLibraryMetadataBatchReceipt {
            requested_photo_count: u32_count(stored.preview.requested_photo_count)?,
            applied_photo_count: u32_count(commands.len())?,
        })
    }
}

fn ffi_gpx_preview(
    preview_id: String,
    preview: &GpsMatchPreview,
) -> AnyResult<ffi::FfiGpxImportPreview> {
    Ok(ffi::FfiGpxImportPreview {
        preview_id,
        source_path: preview.source_path.to_string_lossy().into_owned(),
        source_digest_hex: encode_hex(&preview.source_digest),
        requested_photo_count: u32_count(preview.requested_photo_count)?,
        matched_photo_count: u32_count(preview.proposals.len())?,
        unmatched_photo_count: u32_count(preview.unmatched_photo_count)?,
        proposal_sample: preview
            .proposals
            .iter()
            .take(MAX_GPX_PROPOSAL_SAMPLE)
            .map(|proposal| ffi::FfiGpxMatchProposal {
                photo_id: proposal.photo_id.to_string(),
                captured_at_unix_seconds: proposal.captured_at_unix_seconds,
                matched_at_unix_seconds: proposal.matched_at_unix_seconds,
                nearest_track_delta_seconds: proposal.nearest_track_delta_seconds,
                latitude_e7: proposal.coordinates.latitude_e7,
                longitude_e7: proposal.coordinates.longitude_e7,
            })
            .collect(),
    })
}

fn gpx_source_label(preview: &GpsMatchPreview) -> String {
    let file_name = preview
        .source_path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("GPX");
    let digest = encode_hex(&preview.source_digest);
    format!("{file_name} · {}", &digest[..12])
}

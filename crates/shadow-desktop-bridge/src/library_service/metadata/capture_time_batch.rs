//! Previewed, selection-wide capture-time shifts and override restoration.

use std::collections::{HashSet, VecDeque};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    LibraryMetadataOverrideAction, LibraryMetadataOverrideOrigin, SetPhotoLibraryMetadataOverrides,
};
use shadow_domain::PhotoId;
use uuid::Uuid;

use crate::{ffi, library_service::LibraryService};

use super::{parse_photo_id, u32_count};

const MAX_SELECTION: usize = 100_000;
const MAX_PREVIEWS: usize = 8;
const MAX_PROPOSAL_SAMPLE: usize = 200;
const MAX_SHIFT_SECONDS: i64 = 14 * 86_400;

#[derive(Debug, Clone)]
struct CaptureTimeProposal {
    photo_id: PhotoId,
    before: Option<i64>,
    after: Option<i64>,
    action: LibraryMetadataOverrideAction<i64>,
}

#[derive(Debug)]
struct StoredCaptureTimePreview {
    mode: &'static str,
    offset_seconds: i64,
    requested_photo_count: usize,
    proposals: Vec<CaptureTimeProposal>,
}

#[derive(Debug, Default)]
pub(in crate::library_service) struct CaptureTimePreviewRegistry {
    entries: VecDeque<(String, StoredCaptureTimePreview)>,
}

impl CaptureTimePreviewRegistry {
    fn insert(&mut self, preview: StoredCaptureTimePreview) -> String {
        while self.entries.len() >= MAX_PREVIEWS {
            self.entries.pop_front();
        }
        let id = Uuid::now_v7().to_string();
        self.entries.push_back((id.clone(), preview));
        id
    }

    fn take(&mut self, id: &str) -> Option<StoredCaptureTimePreview> {
        let index = self
            .entries
            .iter()
            .position(|(candidate, _)| candidate == id)?;
        self.entries.remove(index).map(|(_, preview)| preview)
    }
}

impl LibraryService {
    pub(crate) fn preview_capture_time_batch(
        &self,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
        mode: &str,
        offset_seconds: i64,
    ) -> AnyResult<ffi::FfiCaptureTimeBatchPreview> {
        if targets.len() > MAX_SELECTION {
            bail!("capture time selection exceeds {MAX_SELECTION} photos");
        }
        let mode = match mode {
            "shift" => {
                if offset_seconds == 0
                    || !(-MAX_SHIFT_SECONDS..=MAX_SHIFT_SECONDS).contains(&offset_seconds)
                {
                    bail!("capture time shift must be non-zero and within 14 days");
                }
                "shift"
            }
            "inherit" => "inherit",
            _ => bail!("capture time batch mode must be shift or inherit"),
        };
        let mut seen = HashSet::with_capacity(targets.len());
        let mut proposals = Vec::with_capacity(targets.len());
        for target in targets {
            let photo_id = parse_photo_id(&target.photo_id)?;
            if !seen.insert(photo_id) {
                continue;
            }
            let effective = self
                .catalog
                .effective_photo_library_facts(photo_id)?
                .and_then(|facts| facts.captured_at_unix_seconds);
            match mode {
                "shift" => {
                    let Some(before) = effective else {
                        continue;
                    };
                    let after = before
                        .checked_add(offset_seconds)
                        .context("capture time shift overflows the supported timestamp")?;
                    proposals.push(CaptureTimeProposal {
                        photo_id,
                        before: Some(before),
                        after: Some(after),
                        action: LibraryMetadataOverrideAction::Set(after),
                    });
                }
                "inherit" => {
                    let overrides = self.catalog.photo_library_metadata_overrides(photo_id)?;
                    if overrides.capture_time.is_none() {
                        continue;
                    }
                    let observed = self
                        .catalog
                        .photo_library_facts(photo_id)?
                        .and_then(|facts| facts.captured_at_unix_seconds);
                    proposals.push(CaptureTimeProposal {
                        photo_id,
                        before: effective,
                        after: observed,
                        action: LibraryMetadataOverrideAction::Inherit,
                    });
                }
                _ => unreachable!("validated capture time batch mode"),
            }
        }
        let preview = StoredCaptureTimePreview {
            mode,
            offset_seconds: if mode == "shift" { offset_seconds } else { 0 },
            requested_photo_count: seen.len(),
            proposals,
        };
        let ffi_preview = {
            let mut registry = self
                .capture_time_previews
                .lock()
                .map_err(|_| anyhow!("capture time preview registry is unavailable"))?;
            let preview_id = registry.insert(preview);
            let stored = registry
                .entries
                .back()
                .map(|(_, value)| value)
                .expect("inserted preview remains present");
            ffi_preview(preview_id, stored)?
        };
        Ok(ffi_preview)
    }

    pub(crate) fn apply_capture_time_batch(
        &self,
        preview_id: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryMetadataBatchReceipt> {
        let stored = self
            .capture_time_previews
            .lock()
            .map_err(|_| anyhow!("capture time preview registry is unavailable"))?
            .take(preview_id)
            .context("capture time preview expired or was already applied")?;
        let source_label = if stored.mode == "shift" {
            format!("batch-shift:{:+}s", stored.offset_seconds)
        } else {
            String::new()
        };
        let commands = stored
            .proposals
            .iter()
            .map(|proposal| SetPhotoLibraryMetadataOverrides {
                photo_id: proposal.photo_id,
                capture_time: proposal.action.clone(),
                coordinates: LibraryMetadataOverrideAction::Unchanged,
                origin: LibraryMetadataOverrideOrigin::Manual,
                source_label: source_label.clone(),
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
    preview: &StoredCaptureTimePreview,
) -> AnyResult<ffi::FfiCaptureTimeBatchPreview> {
    Ok(ffi::FfiCaptureTimeBatchPreview {
        preview_id,
        mode: preview.mode.into(),
        offset_seconds: preview.offset_seconds,
        requested_photo_count: u32_count(preview.requested_photo_count)?,
        applicable_photo_count: u32_count(preview.proposals.len())?,
        skipped_photo_count: u32_count(
            preview
                .requested_photo_count
                .saturating_sub(preview.proposals.len()),
        )?,
        proposal_sample: preview
            .proposals
            .iter()
            .take(MAX_PROPOSAL_SAMPLE)
            .map(|proposal| ffi::FfiCaptureTimeBatchProposal {
                photo_id: proposal.photo_id.to_string(),
                has_before_capture_time: proposal.before.is_some(),
                before_captured_at_unix_seconds: proposal.before.unwrap_or_default(),
                has_after_capture_time: proposal.after.is_some(),
                after_captured_at_unix_seconds: proposal.after.unwrap_or_default(),
            })
            .collect(),
    })
}

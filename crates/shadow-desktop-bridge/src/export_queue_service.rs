//! Durable export queue orchestration at the desktop bridge boundary.
//!
//! The catalog owns immutable job snapshots and compare-and-swap transitions.
//! This service resolves those snapshots into the renderer's existing exact
//! Recipe path, without letting the Qt shell rediscover mutable photo state.
//! Encoding and atomic file publication remain in the desktop shell because
//! Qt already owns the supported JPEG/PNG and watermark implementations.

use std::collections::BTreeSet;

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{RawDevelopmentPlan, raw_development_plan_identity};
use shadow_catalog::{
    AdvanceExportItem, CatalogHandle, EnqueueExportJob, ExportFailure, ExportItemId,
    ExportItemRecord, ExportItemState, ExportJobId, ExportSettingsSource, NewExportItem,
    NewExportOutputReceipt, RecipeCommitRecord, ReviewItemRecord,
};
use shadow_core::native_path_from_location;
use shadow_domain::{AssetLocation, EntityId, RecipeCommitId};
use uuid::Uuid;

use crate::{digest_hex::encode_hex, wall_clock::current_time_ms};

use crate::{
    DesktopSession, ffi,
    recipe_v1::{
        decode_grade_stack_draft_from_recipe_v1_snapshot, encode_grade_stack_draft_recipe_v1,
    },
};

/// Single semantic owner for the durable catalog queue while `DesktopSession`
/// remains responsible for source validation and rendering.
#[derive(Debug, Clone)]
pub(crate) struct ExportQueueService {
    catalog: CatalogHandle,
}

impl ExportQueueService {
    pub(crate) fn new(catalog: CatalogHandle) -> Self {
        Self { catalog }
    }

    /// Freezes the visible working Recipe of every target before any worker
    /// begins. A photo with no edits receives one neutral autosave commit so
    /// its export is still tied to an immutable Recipe identity.
    pub(crate) fn enqueue(
        &self,
        session: &DesktopSession,
        targets: Vec<ffi::FfiDurableExportTarget>,
        settings_json: &str,
    ) -> AnyResult<ffi::FfiDurableExportJob> {
        let settings_json = normalized_settings_json(settings_json)?;
        if targets.is_empty() {
            bail!("select at least one photo to export");
        }
        let item_count = u32::try_from(targets.len()).unwrap_or(u32::MAX);
        let now_ms = current_time_ms()?;
        let mut outputs = BTreeSet::new();
        let mut items = Vec::with_capacity(targets.len());

        for target in targets {
            let output = validate_export_target(&target)?;
            if !outputs.insert((output.platform.as_str(), output.native_path.clone())) {
                bail!("one export job cannot publish two items to the same destination");
            }
            let (photo_id, source) =
                session.validated_photo_source(&target.photo_id, &target.source_path)?;
            let state = session.photo_edit_state_for(photo_id, &source.location.display_path)?;
            let state = if state.has_working_version {
                state
            } else {
                // A neutral photo still needs a durable, exact source Recipe.
                // This is the same autosave path used by regular editing and
                // deliberately does not create a named Version checkpoint.
                session.autosave_basic_edit_working_at(
                    &target.photo_id,
                    &target.source_path,
                    "",
                    "",
                    &state.settings,
                    now_ms,
                )?
            };
            let recipe_commit_id: RecipeCommitId = state
                .working_commit_id
                .parse()
                .context("parse frozen working Recipe commit for export")?;
            let recipe = self
                .catalog
                .recipe_commit(photo_id, recipe_commit_id)?
                .ok_or_else(|| {
                    anyhow!("frozen export Recipe commit {recipe_commit_id} is absent")
                })?;
            let source_identity_json = source_identity_json(&source)?;
            let render_plan_json = render_plan_json(&recipe)?;
            items.push(NewExportItem {
                photo_id,
                representation_id: source.representation_id,
                recipe_commit_id,
                recipe_snapshot_digest: recipe.snapshot_digest,
                edit_commit_id: None,
                source_identity_json,
                render_plan_json,
                output,
            });
        }

        let job = self.catalog.enqueue_export_job(&EnqueueExportJob {
            settings: ExportSettingsSource::InlineJson(settings_json),
            items,
            now_ms,
        })?;
        Ok(ffi::FfiDurableExportJob {
            job_id: job.id.to_string(),
            item_count,
        })
    }

    /// Converts in-flight work left by a prior process into queued work. The
    /// prior process never receives credit for an output unless it reached the
    /// atomic publication receipt, so automatic resume is safe here.
    pub(crate) fn recover_for_startup(&self) -> AnyResult<ffi::FfiDurableExportRecovery> {
        // This is one Catalog transaction over the entire queue. In
        // particular, never page task-center jobs here: a startup recovery
        // must not silently strand the 257th job after a prior crash.
        let recovery = self
            .catalog
            .recover_and_requeue_interrupted_export_items(current_time_ms()?)?;
        Ok(ffi::FfiDurableExportRecovery {
            interrupted_items: saturating_u32(recovery.interrupted_item_count),
            requeued_items: saturating_u32(recovery.requeued_item_count),
            queued_items: saturating_u32(recovery.queued_item_count),
        })
    }

    /// Claims one globally oldest queue item. The global ordering lets a
    /// restart resume prior work before a newly-created job, while retaining
    /// deterministic progress inside every job.
    pub(crate) fn claim_next(&self) -> AnyResult<Option<ffi::FfiDurableExportItem>> {
        loop {
            let Some(item) = self.catalog.claim_next_export_item(current_time_ms()?)? else {
                return Ok(None);
            };
            let source = self.catalog.photo_source(item.photo_id)?;
            let Some(source) =
                source.filter(|source| source.representation_id == item.representation_id)
            else {
                self.fail(
                    item.id,
                    ExportItemState::Preparing,
                    "export_source_unavailable",
                    "the photo's frozen source is no longer online at its registered location",
                    true,
                )?;
                continue;
            };
            let job = self
                .catalog
                .export_job(item.job_id)?
                .ok_or_else(|| anyhow!("durable export job {} is absent", item.job_id))?;
            if native_path_from_location(&item.output).is_err() {
                self.fail(
                    item.id,
                    ExportItemState::Preparing,
                    "export_output_platform_mismatch",
                    "the frozen export destination cannot be reopened on this platform",
                    false,
                )?;
                continue;
            }
            return Ok(Some(ffi::FfiDurableExportItem {
                has_item: true,
                item_id: item.id.to_string(),
                job_id: item.job_id.to_string(),
                photo_id: item.photo_id.to_string(),
                source_path: source.location.display_path,
                output_path: crate::native_path_ffi::location_to_ffi(&item.output)?,
                settings_json: job.settings_json,
            }));
        }
    }

    pub(crate) fn begin_render(&self, item_id: &str) -> AnyResult<()> {
        self.transition(
            item_id,
            ExportItemState::Preparing,
            ExportItemState::Rendering,
        )
    }

    pub(crate) fn begin_encoding(&self, item_id: &str) -> AnyResult<()> {
        self.transition(
            item_id,
            ExportItemState::Rendering,
            ExportItemState::Encoding,
        )
    }

    pub(crate) fn begin_writing(&self, item_id: &str) -> AnyResult<()> {
        self.transition(
            item_id,
            ExportItemState::Encoding,
            ExportItemState::WritingTemp,
        )
    }

    pub(crate) fn pause_for_conflict(&self, item_id: &str) -> AnyResult<()> {
        self.transition(
            item_id,
            ExportItemState::WritingTemp,
            ExportItemState::PausedConflict,
        )
    }

    /// Uses the immutable Recipe commit recorded in the queue item. It never
    /// asks for the photo's current working ref, even when edits changed after
    /// the job was queued.
    pub(crate) fn render(
        &self,
        session: &DesktopSession,
        item: &ffi::FfiDurableExportItem,
    ) -> AnyResult<ffi::FfiEditedExportRaster> {
        let item = self.persisted_item(item)?;
        if item.state != ExportItemState::Rendering {
            bail!(
                "durable export item {} is {}, not rendering",
                item.id,
                item.state.as_str()
            );
        }
        let source = self
            .catalog
            .photo_source(item.photo_id)?
            .filter(|source| source.representation_id == item.representation_id)
            .ok_or_else(|| anyhow!("durable export source is no longer available"))?;
        let recipe = self
            .catalog
            .recipe_commit(item.photo_id, item.recipe_commit_id)?
            .ok_or_else(|| {
                anyhow!(
                    "frozen export Recipe commit {} is absent",
                    item.recipe_commit_id
                )
            })?;
        if recipe.snapshot_digest != item.recipe_snapshot_digest {
            bail!("frozen export Recipe digest no longer matches its immutable commit");
        }
        let grade_stack =
            decode_grade_stack_draft_from_recipe_v1_snapshot(recipe.commit.snapshot())
                .context("decode frozen export Recipe graph")?;
        session.render_basic_edit_export(
            &item.photo_id.to_string(),
            &source.location.display_path,
            &ffi::FfiEditExportRequest {
                base_commit_id: item.recipe_commit_id.to_string(),
                settings: encode_grade_stack_draft_recipe_v1(grade_stack)
                    .context("project frozen export Recipe into the desktop render request")?,
                // The renderer treats the explicit commit id as immutable;
                // this flag retains its Recipe template/provenance handling.
                use_working_recipe: true,
            },
        )
    }

    pub(crate) fn complete(
        &self,
        item_id: &str,
        job_id: &str,
        output_format: &str,
        byte_len: u64,
        receipt_json: &str,
    ) -> AnyResult<()> {
        let item_id = parse_item_id(item_id)?;
        let item = self.item_by_id_for_job(parse_job_id(job_id)?, item_id)?;
        self.catalog.advance_export_item(&AdvanceExportItem {
            item_id,
            expected_state: ExportItemState::WritingTemp,
            next_state: ExportItemState::Completed,
            failure: None,
            receipt: Some(NewExportOutputReceipt {
                output: item.output,
                output_format: output_format.to_owned(),
                byte_len,
                // A future output verifier can enrich this receipt with a
                // whole-file digest without changing the publication state.
                content_digest: None,
                receipt_json: receipt_json.to_owned(),
            }),
            now_ms: current_time_ms()?,
        })?;
        Ok(())
    }

    pub(crate) fn fail_from_ffi(
        &self,
        item_id: &str,
        stage: ffi::FfiDurableExportItemState,
        code: &str,
        message: &str,
        retryable: bool,
    ) -> AnyResult<()> {
        self.fail(
            parse_item_id(item_id)?,
            item_state_from_ffi(stage)?,
            code,
            message,
            retryable,
        )
    }

    pub(crate) fn cancel_job(&self, job_id: &str) -> AnyResult<()> {
        let job_id = parse_job_id(job_id)?;
        self.catalog.cancel_export_job(job_id, current_time_ms()?)?;
        Ok(())
    }

    pub(crate) fn progress(&self, job_id: &str) -> AnyResult<ffi::FfiDurableExportProgress> {
        let job_id = parse_job_id(job_id)?;
        let progress = self.catalog.export_job_progress(job_id)?;
        Ok(ffi::FfiDurableExportProgress {
            // An interrupted row can only persist before startup recovery;
            // expose it as resumable work rather than pretending it vanished
            // from the task-center total.
            queued: saturating_u32(progress.resumable_item_count()),
            active: saturating_u32(progress.active_item_count()),
            completed: saturating_u32(progress.completed_item_count),
            failed: saturating_u32(progress.failed_item_count),
            cancelled: saturating_u32(progress.cancelled_item_count),
            paused_conflict: saturating_u32(progress.paused_conflict_item_count),
            total: saturating_u32(progress.total_item_count),
        })
    }

    fn transition(
        &self,
        item_id: &str,
        expected_state: ExportItemState,
        next_state: ExportItemState,
    ) -> AnyResult<()> {
        self.catalog.advance_export_item(&AdvanceExportItem {
            item_id: parse_item_id(item_id)?,
            expected_state,
            next_state,
            failure: None,
            receipt: None,
            now_ms: current_time_ms()?,
        })?;
        Ok(())
    }

    fn fail(
        &self,
        item_id: ExportItemId,
        expected_state: ExportItemState,
        code: &str,
        message: &str,
        retryable: bool,
    ) -> AnyResult<()> {
        let code = code.trim();
        let message = message.trim();
        if code.is_empty() || message.is_empty() {
            bail!("durable export failures require a stable code and message");
        }
        self.catalog.advance_export_item(&AdvanceExportItem {
            item_id,
            expected_state,
            next_state: ExportItemState::Failed,
            failure: Some(ExportFailure {
                code: code.to_owned(),
                message: message.to_owned(),
                retryable,
            }),
            receipt: None,
            now_ms: current_time_ms()?,
        })?;
        Ok(())
    }

    fn persisted_item(&self, item: &ffi::FfiDurableExportItem) -> AnyResult<ExportItemRecord> {
        let item_id = parse_item_id(&item.item_id)?;
        let job_id = parse_job_id(&item.job_id)?;
        let persisted = self.item_by_id_for_job(job_id, item_id)?;
        let received_output = crate::native_path_ffi::location_from_ffi(&item.output_path)?;
        if persisted.photo_id.to_string() != item.photo_id || persisted.output != received_output {
            bail!("durable export item identity changed after it was claimed");
        }
        Ok(persisted)
    }

    fn item_by_id_for_job(
        &self,
        job_id: ExportJobId,
        item_id: ExportItemId,
    ) -> AnyResult<ExportItemRecord> {
        let item = self
            .catalog
            .export_item(item_id)?
            .ok_or_else(|| anyhow!("durable export item {item_id} is absent"))?;
        if item.job_id != job_id {
            bail!(
                "durable export item {item_id} belongs to job {}, not {job_id}",
                item.job_id
            );
        }
        Ok(item)
    }
}

fn saturating_u32(value: u64) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

fn validate_export_target(target: &ffi::FfiDurableExportTarget) -> AnyResult<AssetLocation> {
    if target.photo_id.trim().is_empty() || target.source_path.trim().is_empty() {
        bail!("durable export targets require photo id, source path, and output path");
    }
    let output = crate::native_path_ffi::location_from_ffi(&target.output_path)?;
    let output_path = native_path_from_location(&output)?;
    if output_path.as_os_str().is_empty() {
        bail!("durable export targets require photo id, source path, and output path");
    }
    if !output_path.is_absolute() {
        bail!("durable export output paths must be absolute");
    }
    Ok(output)
}

fn normalized_settings_json(settings_json: &str) -> AnyResult<String> {
    let value: serde_json::Value =
        serde_json::from_str(settings_json).context("parse durable export settings JSON")?;
    if !value.is_object() {
        bail!("durable export settings must be one JSON object");
    }
    serde_json::to_string(&value).context("serialize durable export settings JSON")
}

fn source_identity_json(source: &ReviewItemRecord) -> AnyResult<String> {
    serde_json::to_string(&serde_json::json!({
        "schema": 1,
        "representation_id": source.representation_id.to_string(),
        "byte_len": source.source.byte_len,
        "modified_at_ms": source.source.modified_at_ms,
    }))
    .context("serialize frozen export source identity")
}

fn render_plan_json(recipe: &RecipeCommitRecord) -> AnyResult<String> {
    let raw_plan_identity = raw_development_plan_identity(RawDevelopmentPlan::export_image())
        .context("identify frozen export RAW development plan")?;
    serde_json::to_string(&serde_json::json!({
        "schema": 1,
        "renderer": "shadow-desktop-bridge/export-queue-v1",
        "recipe_commit_id": recipe.commit.id().to_string(),
        "recipe_snapshot_digest": encode_hex(&recipe.snapshot_digest),
        "raw_development_plan": raw_plan_identity,
    }))
    .context("serialize frozen export render plan")
}

fn parse_item_id(value: &str) -> AnyResult<ExportItemId> {
    Ok(ExportItemId::from_uuid(
        Uuid::parse_str(value)
            .with_context(|| format!("parse durable export item id {value:?}"))?,
    ))
}

fn parse_job_id(value: &str) -> AnyResult<ExportJobId> {
    Ok(ExportJobId::from_uuid(
        Uuid::parse_str(value).with_context(|| format!("parse durable export job id {value:?}"))?,
    ))
}

fn item_state_from_ffi(value: ffi::FfiDurableExportItemState) -> AnyResult<ExportItemState> {
    Ok(match value {
        ffi::FfiDurableExportItemState::Preparing => ExportItemState::Preparing,
        ffi::FfiDurableExportItemState::Rendering => ExportItemState::Rendering,
        ffi::FfiDurableExportItemState::Encoding => ExportItemState::Encoding,
        ffi::FfiDurableExportItemState::WritingTemp => ExportItemState::WritingTemp,
        _ => bail!("unknown durable export item state received from desktop shell"),
    })
}

#[cfg(test)]
mod tests;

//! Durable publication of a completed, settled Recipe preview.
//!
//! The render lifecycle owns admission and terminal linearization. This module
//! starts only after that boundary and owns the complete rebuildable storage
//! transaction: execution identities, variant and generator identity, blob
//! storage, and the source-checked Catalog reference.

use std::{
    sync::{OnceLock, mpsc},
    thread,
};

use anyhow::{Context, Result as AnyResult};
use shadow_bridge::{
    EditPreviewExecutionReceipt, RawDevelopmentPlan, RawPipelineReceipt,
    edit_preview_generator_implementation_identity, raw_development_plan_identity,
};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogHandle, RecordCachedArtifact,
    RepresentationFingerprint,
};
use shadow_core::CachedArtifactLoader;
use shadow_domain::{PreviewByteOrder, PreviewCodec, ProxyPayload, RepresentationId};

use crate::{
    digest_hex::encode_hex,
    preview_cache_identity::{
        EDIT_PREVIEW_GENERATOR_ID, edit_preview_generator_version,
        prepared_edit_execution_cache_identity, prepared_raw_pipeline_cache_identity,
    },
    wall_clock::current_time_ms,
};

#[derive(Debug, Clone, Copy)]
pub(crate) struct RecipePreviewStoreRequest<'a> {
    pub(crate) representation_id: RepresentationId,
    pub(crate) expected_source: RepresentationFingerprint,
    pub(crate) proxy: &'a ProxyPayload,
    pub(crate) recipe_snapshot_digest: [u8; 32],
    pub(crate) max_edge: u32,
    pub(crate) jpeg_quality: u8,
    pub(crate) raw_development_plan: RawDevelopmentPlan,
    pub(crate) raw_pipeline_receipt: &'a RawPipelineReceipt,
    pub(crate) edit_execution_receipt: &'a EditPreviewExecutionReceipt,
    pub(crate) source_environment_cache_identity: &'a str,
}

#[derive(Debug)]
pub(crate) struct RecipePreviewStoreJob {
    pub(crate) catalog: CatalogHandle,
    pub(crate) loader: CachedArtifactLoader,
    pub(crate) representation_id: RepresentationId,
    pub(crate) expected_source: RepresentationFingerprint,
    pub(crate) proxy: ProxyPayload,
    pub(crate) recipe_snapshot_digest: [u8; 32],
    pub(crate) max_edge: u32,
    pub(crate) jpeg_quality: u8,
    pub(crate) raw_development_plan: RawDevelopmentPlan,
    pub(crate) raw_pipeline_receipt: RawPipelineReceipt,
    pub(crate) edit_execution_receipt: EditPreviewExecutionReceipt,
    pub(crate) source_environment_cache_identity: String,
}

impl RecipePreviewStoreJob {
    pub(crate) fn store(&self) -> AnyResult<()> {
        store_recipe_preview(
            &self.catalog,
            &self.loader,
            RecipePreviewStoreRequest {
                representation_id: self.representation_id,
                expected_source: self.expected_source,
                proxy: &self.proxy,
                recipe_snapshot_digest: self.recipe_snapshot_digest,
                max_edge: self.max_edge,
                jpeg_quality: self.jpeg_quality,
                raw_development_plan: self.raw_development_plan,
                raw_pipeline_receipt: &self.raw_pipeline_receipt,
                edit_execution_receipt: &self.edit_execution_receipt,
                source_environment_cache_identity: &self.source_environment_cache_identity,
            },
        )
    }
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum RecipePreviewStoreEnqueue {
    Queued,
    Dropped,
}

// Durable previews are rebuildable and must never hold the completed pixels
// hostage. Keep a small bounded queue: normal settled edits are preserved,
// while an unavailable or saturated cache drops work instead of propagating
// storage latency back into the interactive renderer and Qt publication path.
const RECIPE_PREVIEW_STORE_QUEUE_CAPACITY: usize = 4;
static RECIPE_PREVIEW_STORE_SENDER: OnceLock<Option<mpsc::SyncSender<RecipePreviewStoreJob>>> =
    OnceLock::new();

fn recipe_preview_store_sender() -> Option<&'static mpsc::SyncSender<RecipePreviewStoreJob>> {
    RECIPE_PREVIEW_STORE_SENDER
        .get_or_init(|| {
            let (sender, receiver) =
                mpsc::sync_channel::<RecipePreviewStoreJob>(RECIPE_PREVIEW_STORE_QUEUE_CAPACITY);
            match thread::Builder::new()
                .name("shadow-recipe-preview-cache".to_owned())
                .spawn(move || {
                    while let Ok(job) = receiver.recv() {
                        if let Err(error) = job.store() {
                            eprintln!("Shadow: could not cache edited preview: {error:#}");
                        }
                    }
                }) {
                Ok(_) => Some(sender),
                Err(error) => {
                    eprintln!("Shadow: could not start edited-preview cache worker: {error}");
                    None
                }
            }
        })
        .as_ref()
}

fn try_enqueue_rebuildable_cache_job<T>(
    sender: &mpsc::SyncSender<T>,
    job: T,
) -> RecipePreviewStoreEnqueue {
    match sender.try_send(job) {
        Ok(()) => RecipePreviewStoreEnqueue::Queued,
        Err(mpsc::TrySendError::Full(_)) => {
            eprintln!(
                "Shadow: edited-preview cache queue is full; skipping rebuildable cache write"
            );
            RecipePreviewStoreEnqueue::Dropped
        }
        Err(mpsc::TrySendError::Disconnected(_)) => {
            eprintln!("Shadow: edited-preview cache worker is unavailable");
            RecipePreviewStoreEnqueue::Dropped
        }
    }
}

pub(crate) fn defer_recipe_preview_store(job: RecipePreviewStoreJob) -> RecipePreviewStoreEnqueue {
    let Some(sender) = recipe_preview_store_sender() else {
        return RecipePreviewStoreEnqueue::Dropped;
    };
    try_enqueue_rebuildable_cache_job(sender, job)
}

/// Persists one rendered edit preview with exact source, Recipe, RAW, edit,
/// display, and generator provenance.
///
/// The caller has already won the unique completed terminal claim for a
/// settled render. A stale source can therefore leave an unreferenced blob,
/// but Catalog will never attach it to newer source bytes.
pub(crate) fn store_recipe_preview(
    catalog: &CatalogHandle,
    loader: &CachedArtifactLoader,
    request: RecipePreviewStoreRequest<'_>,
) -> AnyResult<()> {
    if request.raw_pipeline_receipt.requested_plan != request.raw_development_plan {
        anyhow::bail!(
            "Recipe preview publication RAW plan does not match its prepared-source receipt"
        );
    }
    let raw_pipeline = prepared_raw_pipeline_cache_identity(request.raw_pipeline_receipt)
        .context("identify prepared Recipe-preview source pipeline")?;
    let edit_execution = prepared_edit_execution_cache_identity(request.edit_execution_receipt)
        .context("identify completed Recipe-preview edit/display execution")?;
    let raw_plan_identity = raw_development_plan_identity(request.raw_development_plan)
        .context("build Recipe-preview RAW-development cache identity")?;
    let variant_key = format!(
        "shadow-recipe-preview:jpeg-{}-q{}-444-v1;{raw_plan_identity};\
         pipeline={};execution={};recipe={}",
        request.max_edge,
        request.jpeg_quality,
        raw_pipeline.component(),
        edit_execution.component(),
        encode_hex(&request.recipe_snapshot_digest)
    );
    let blob = loader
        .store_bytes(&request.proxy.bytes)
        .context("store rendered Recipe preview blob")?;
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: request.representation_id,
            expected_source: request.expected_source,
            artifact: CachedArtifact {
                role: CachedArtifactRole::RecipePreview,
                variant_key,
                generator_id: EDIT_PREVIEW_GENERATOR_ID.to_owned(),
                generator_version: edit_preview_generator_version(
                    request.source_environment_cache_identity,
                    &edit_preview_generator_implementation_identity(),
                ),
                recipe_snapshot_digest: Some(request.recipe_snapshot_digest),
                provider_preview_id: None,
                blob_algorithm: blob.digest.algorithm().to_owned(),
                blob_digest: *blob.digest.as_bytes(),
                blob_byte_len: blob.byte_len,
                codec: PreviewCodec::Jpeg,
                byte_order: PreviewByteOrder::NotApplicable,
                dimensions: request.proxy.dimensions,
                bits_per_channel: request.proxy.bits_per_channel,
                channels: request.proxy.channels,
                created_at_ms: current_time_ms()?,
            },
        })
        .context("record rendered Recipe preview provenance")?;
    Ok(())
}

#[cfg(test)]
mod tests;

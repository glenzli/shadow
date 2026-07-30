//! Durable publication of a completed, settled Recipe preview.
//!
//! The render lifecycle owns admission and terminal linearization. This module
//! starts only after that boundary and owns the complete rebuildable storage
//! transaction: execution identities, variant and generator identity, blob
//! storage, and the source-checked Catalog reference.

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

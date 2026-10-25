//! Exact generated-proxy admission before expensive source development.
//!
//! Reuse never changes pixel semantics or cache identity. A verified existing
//! proxy can satisfy repeated work or analysis backfill; an embedded/Recipe
//! visual, different source, renderer, or variant cannot substitute for it.

use std::{fs, io};

use shadow_cache::{BlobDigest, CacheError};
use shadow_catalog::CachedArtifactRole;

use crate::performance::{DecodePerformance, measure_if};

use super::{
    DecodeInspectionDiscardReason, DecodeInspector, PreviewCacheOutcome, VisualCacheContext,
    cancelled_preview, source_changed_profiled,
};

pub(in crate::decode_inspection) fn reuse_generated_proxy(
    inspector: &impl DecodeInspector,
    context: VisualCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Option<PreviewCacheOutcome> {
    try_reuse(inspector, context, performance)
        .unwrap_or_else(|message| Some(PreviewCacheOutcome::Failed(message)))
}

fn try_reuse(
    inspector: &impl DecodeInspector,
    context: VisualCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<Option<PreviewCacheOutcome>, String> {
    if context.cancellation.is_cancelled() {
        return Ok(Some(cancelled_preview()));
    }
    let records = context
        .catalog
        .cached_artifacts(context.request.representation_id)
        .map_err(|error| error.to_string())?;
    let Some(record) = records.into_iter().find(|record| {
        let artifact = &record.artifact;
        record.source == context.request.expected_source
            && artifact.role == CachedArtifactRole::GeneratedProxy
            && artifact.variant_key == inspector.proxy_variant_key()
            && artifact.generator_id == context.provider_id
            && artifact.generator_version == context.provider_version
            && artifact.recipe_snapshot_digest.is_none()
    }) else {
        return Ok(None);
    };
    let artifact = &record.artifact;
    let digest = BlobDigest::from_bytes(artifact.blob_digest);
    if artifact.blob_algorithm != digest.algorithm() {
        return Ok(None);
    }
    // Verification streams through a bounded buffer, never reads RAW, and does
    // not retain another image in either CPU or GPU memory.
    let valid = match context.cache.verify(digest) {
        Ok(()) => fs::metadata(context.cache.resolve(digest))
            .map(|metadata| metadata.len() == artifact.blob_byte_len)
            .map_err(|error| error.to_string())?,
        Err(CacheError::CorruptBlob(_)) => {
            context
                .cache
                .quarantine_corrupt(digest)
                .map_err(|error| error.to_string())?;
            false
        }
        Err(CacheError::Io { source, .. }) if source.kind() == io::ErrorKind::NotFound => false,
        Err(error) => return Err(error.to_string()),
    };
    if !valid {
        // Exact-row invalidation cannot delete a newer worker's replacement.
        context
            .catalog
            .invalidate_cached_artifact(&record)
            .map_err(|error| error.to_string())?;
        return Ok(None);
    }
    if context.cancellation.is_cancelled() {
        return Ok(Some(cancelled_preview()));
    }
    if source_changed_profiled(context.request, performance) {
        return Ok(Some(PreviewCacheOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        )));
    }
    if context
        .catalog
        .representation_fingerprint(context.request.representation_id)
        .map_err(|error| error.to_string())?
        != context.request.expected_source
    {
        return Ok(Some(PreviewCacheOutcome::Discarded(
            DecodeInspectionDiscardReason::CatalogChanged,
        )));
    }
    if let Some(observer) = context.technical_observer {
        measure_if(
            performance.profiled,
            &mut performance.technical_submit_wait,
            || {
                observer.submit_preferred_detached(
                    context.request.representation_id,
                    context.request.expected_source,
                )
            },
        )
        .map_err(|error| error.to_string())?;
    }
    // No Catalog rewrite, cache publication, or visual-publication increment:
    // this reports work avoided, not a newly rendered image.
    Ok(Some(PreviewCacheOutcome::ReusedGeneratedProxy {
        digest_hex: digest.to_hex(),
        byte_len: artifact.blob_byte_len,
        dimensions: artifact.dimensions,
    }))
}

#[cfg(test)]
mod tests;

//! Embedded-preview publication and generated-proxy persistence.

use std::sync::Mutex;

use shadow_cache::{ContentAddressedStore, StoredBlob};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogHandle, RecordCachedArtifact,
    RecordCachedArtifactStatus,
};
use shadow_domain::{ImageDimensions, PreviewByteOrder, PreviewPayload};

use crate::import::ScanCancellation;
use crate::performance::{DecodePerformance, measure_if};
use crate::technical_observation::TechnicalObservationHandle;

use super::contract::{
    DecodeInspectionDiscardReason, DecodeInspectionRequest, DecodeInspector,
    EmbeddedPreviewPublication, EmbeddedPreviewSink, PreviewCacheOutcome,
};
use super::runtime_state::DecodeInspectionState;
use super::source_identity::{now_ms, source_changed_profiled};

/// Inputs shared by the embedded-preview and generated-proxy paths.
///
/// Keeping this transaction context intact makes cancellation and source
/// identity checks impossible to omit from either publication strategy.
#[derive(Copy, Clone)]
pub(super) struct VisualCacheContext<'a> {
    pub(super) catalog: &'a CatalogHandle,
    pub(super) cache: &'a ContentAddressedStore,
    pub(super) technical_observer: Option<&'a TechnicalObservationHandle>,
    pub(super) embedded_preview_sink: Option<&'a dyn EmbeddedPreviewSink>,
    pub(super) request: &'a DecodeInspectionRequest,
    pub(super) provider_id: &'a str,
    pub(super) provider_version: &'a str,
    pub(super) cancellation: &'a ScanCancellation,
}

pub(super) fn cache_preview(
    inspector: &mut impl DecodeInspector,
    context: VisualCacheContext<'_>,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    // A camera preview is the fast path to a populated Library, not the final
    // Shadow rendering contract. Desktop sessions may keep it only in memory;
    // callers without a sink retain the legacy durable-cache behavior. In both
    // cases a generated proxy continues from the same source and replaces the
    // temporary visual when its deterministic render is recorded.
    let mut fallback = None;
    match extract_embedded_preview(inspector, context, performance) {
        Ok(Some(preview)) => {
            let outcome = if let Some(sink) = context.embedded_preview_sink {
                publish_embedded_preview(context, sink, preview, inspection_state, performance)
            } else {
                let prepared = prepare_embedded_cached_visual(context, preview);
                store_prepared_cached_visual(context, prepared, inspection_state, performance)
            };
            if matches!(outcome, PreviewCacheOutcome::Discarded(_)) {
                return outcome;
            }
            fallback = Some(outcome);
        }
        Ok(None) => {}
        Err(outcome) => {
            if matches!(outcome, PreviewCacheOutcome::Discarded(_)) {
                return outcome;
            }
            fallback = Some(outcome);
        }
    }

    match prepare_generated_proxy(inspector, context, performance) {
        Ok(Some(prepared)) => {
            let outcome =
                store_prepared_cached_visual(context, prepared, inspection_state, performance);
            match outcome {
                PreviewCacheOutcome::StoredGeneratedProxy { .. }
                | PreviewCacheOutcome::Discarded(_) => outcome,
                failed => fallback.unwrap_or(failed),
            }
        }
        Ok(None) => fallback.unwrap_or(PreviewCacheOutcome::NoVisualAvailable),
        Err(outcome) => {
            if matches!(outcome, PreviewCacheOutcome::Discarded(_)) {
                outcome
            } else {
                fallback.unwrap_or(outcome)
            }
        }
    }
}

type PreparedCachedVisual = (Vec<u8>, CachedArtifact, CachedVisualKind);

fn store_prepared_cached_visual(
    context: VisualCacheContext<'_>,
    prepared: PreparedCachedVisual,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    let (bytes, artifact, stored_kind) = prepared;
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    let blob = match measure_if(
        performance.profiled,
        &mut performance.cache_blob_put,
        || context.cache.put(&bytes),
    ) {
        Ok(blob) => blob,
        Err(error) => return PreviewCacheOutcome::Failed(error.to_string()),
    };
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    let artifact = CachedArtifact {
        blob_algorithm: blob.digest.algorithm().to_owned(),
        blob_digest: *blob.digest.as_bytes(),
        blob_byte_len: blob.byte_len,
        created_at_ms: now_ms(),
        ..artifact
    };
    let outcome = record_cached_visual(
        context.catalog,
        context.technical_observer,
        context.request,
        artifact,
        stored_kind,
        &blob,
        performance,
    );
    count_published_visual(&outcome, inspection_state);
    outcome
}

fn count_published_visual(
    outcome: &PreviewCacheOutcome,
    inspection_state: &Mutex<DecodeInspectionState>,
) {
    if matches!(
        outcome,
        PreviewCacheOutcome::PublishedEmbeddedPreview { .. }
            | PreviewCacheOutcome::StoredEmbeddedPreview { .. }
            | PreviewCacheOutcome::StoredGeneratedProxy { .. }
    ) {
        let mut state = inspection_state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        state.visual_artifacts_published = state.visual_artifacts_published.saturating_add(1);
    }
}

fn publish_embedded_preview(
    context: VisualCacheContext<'_>,
    sink: &dyn EmbeddedPreviewSink,
    preview: PreviewPayload,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    let byte_len = u64::try_from(preview.bytes.len()).unwrap_or(u64::MAX);
    let outcome = match sink.publish_embedded_preview(EmbeddedPreviewPublication {
        representation_id: context.request.representation_id,
        source: context.request.expected_source,
        preview,
    }) {
        Ok(()) => PreviewCacheOutcome::PublishedEmbeddedPreview { byte_len },
        Err(error) => PreviewCacheOutcome::Failed(error),
    };
    count_published_visual(&outcome, inspection_state);
    outcome
}

fn extract_embedded_preview(
    inspector: &mut impl DecodeInspector,
    context: VisualCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<Option<PreviewPayload>, PreviewCacheOutcome> {
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let preview = measure_if(
        performance.profiled,
        &mut performance.embedded_preview_extract,
        || inspector.extract_best_preview(&context.request.path),
    )
    .map_err(PreviewCacheOutcome::Failed)?;
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    if source_changed_profiled(context.request, performance) {
        return Err(PreviewCacheOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }
    Ok(preview)
}

fn prepare_embedded_cached_visual(
    context: VisualCacheContext<'_>,
    preview: PreviewPayload,
) -> PreparedCachedVisual {
    let artifact = CachedArtifact {
        role: CachedArtifactRole::EmbeddedPreview,
        variant_key: context.provider_id.to_owned(),
        generator_id: context.provider_id.to_owned(),
        generator_version: context.provider_version.to_owned(),
        recipe_snapshot_digest: None,
        provider_preview_id: Some(preview.descriptor.provider_id),
        blob_algorithm: String::new(),
        blob_digest: [0; 32],
        blob_byte_len: 0,
        codec: preview.descriptor.codec,
        byte_order: preview.byte_order,
        dimensions: preview.descriptor.dimensions,
        bits_per_channel: preview.descriptor.bits_per_channel,
        channels: preview.descriptor.channels,
        created_at_ms: 0,
    };
    (preview.bytes, artifact, CachedVisualKind::EmbeddedPreview)
}

fn prepare_generated_proxy(
    inspector: &mut impl DecodeInspector,
    context: VisualCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<Option<PreparedCachedVisual>, PreviewCacheOutcome> {
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let proxy = measure_if(performance.profiled, &mut performance.proxy_render, || {
        inspector.render_proxy(&context.request.path)
    })
    .map_err(PreviewCacheOutcome::Failed)?;
    let Some(proxy) = proxy else {
        return Ok(None);
    };
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let dimensions = proxy.dimensions;
    let artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: inspector.proxy_variant_key().to_owned(),
        generator_id: context.provider_id.to_owned(),
        generator_version: context.provider_version.to_owned(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: String::new(),
        blob_digest: [0; 32],
        blob_byte_len: 0,
        codec: proxy.codec,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions,
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        created_at_ms: 0,
    };
    Ok(Some((
        proxy.bytes,
        artifact,
        CachedVisualKind::GeneratedProxy(dimensions),
    )))
}

pub(super) fn cancelled_outcome() -> super::contract::DecodeInspectionOutcome {
    super::contract::DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
}

fn cancelled_preview() -> PreviewCacheOutcome {
    PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
}

fn record_cached_visual(
    catalog: &CatalogHandle,
    technical_observer: Option<&TechnicalObservationHandle>,
    request: &DecodeInspectionRequest,
    artifact: CachedArtifact,
    stored_kind: CachedVisualKind,
    blob: &StoredBlob,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    let status = measure_if(
        performance.profiled,
        &mut performance.cache_artifact_catalog_commit,
        || {
            catalog.record_cached_artifact(&RecordCachedArtifact {
                representation_id: request.representation_id,
                expected_source: request.expected_source,
                artifact,
            })
        },
    );
    match status {
        Ok(RecordCachedArtifactStatus::Recorded) => {
            if let Some(observer) = technical_observer {
                let _ = measure_if(
                    performance.profiled,
                    &mut performance.technical_submit_wait,
                    || {
                        observer.submit_preferred_detached(
                            request.representation_id,
                            request.expected_source,
                        )
                    },
                );
            }
            match stored_kind {
                CachedVisualKind::EmbeddedPreview => PreviewCacheOutcome::StoredEmbeddedPreview {
                    digest_hex: blob.digest.to_hex(),
                    byte_len: blob.byte_len,
                },
                CachedVisualKind::GeneratedProxy(dimensions) => {
                    PreviewCacheOutcome::StoredGeneratedProxy {
                        digest_hex: blob.digest.to_hex(),
                        byte_len: blob.byte_len,
                        dimensions,
                    }
                }
            }
        }
        Ok(RecordCachedArtifactStatus::StaleSource) => {
            PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::CatalogChanged)
        }
        Err(error) => PreviewCacheOutcome::Failed(error.to_string()),
    }
}

#[derive(Debug, Copy, Clone)]
enum CachedVisualKind {
    EmbeddedPreview,
    GeneratedProxy(ImageDimensions),
}

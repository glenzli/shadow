//! One decode inspection transaction executed by a pool worker.

use std::sync::{Mutex, mpsc::Receiver};

use shadow_cache::ContentAddressedStore;
use shadow_catalog::{CatalogHandle, RecordDecodeSnapshot, RecordDecodeSnapshotStatus};

use crate::import::ScanCancellation;
use crate::performance::{DecodePerformance, measure_if};
use crate::technical_observation::TechnicalObservationHandle;

use super::contract::{
    DecodeInspectionDiscardReason, DecodeInspectionError, DecodeInspectionOutcome,
    DecodeInspectionRequest, DecodeInspector, EmbeddedPreviewSink, PreviewCacheOutcome,
};
use super::runtime_state::DecodeInspectionState;
use super::source_identity::{now_ms, read_source_fingerprint_profiled};
use super::submission::Message;
use super::visual_cache::{VisualCacheContext, cache_preview, cancelled_outcome};

/// Immutable services shared by every transaction on one worker thread.
#[derive(Copy, Clone)]
pub(super) struct WorkerContext<'a> {
    pub(super) catalog: &'a CatalogHandle,
    pub(super) cache: Option<&'a ContentAddressedStore>,
    pub(super) technical_observer: Option<&'a TechnicalObservationHandle>,
    pub(super) embedded_preview_sink: Option<&'a dyn EmbeddedPreviewSink>,
    pub(super) state: &'a Mutex<DecodeInspectionState>,
    pub(super) profiled: bool,
}

pub(super) fn run_worker(
    mut inspector: impl DecodeInspector,
    receiver: &Receiver<Message>,
    context: WorkerContext<'_>,
) {
    let mut performance = DecodePerformance {
        profiled: context.profiled,
        ..DecodePerformance::default()
    };
    while let Ok(message) = receiver.recv() {
        match message {
            Message::Inspect(inspection) => {
                if let Some(enqueued_at) = inspection.enqueued_at {
                    performance.queue_wait.record(enqueued_at.elapsed());
                }
                let result = if inspection.cancellation.is_cancelled() {
                    Ok(DecodeInspectionOutcome::Discarded(
                        DecodeInspectionDiscardReason::Cancelled,
                    ))
                } else {
                    inspect_and_record(
                        &mut inspector,
                        &inspection.request,
                        &inspection.cancellation,
                        context,
                        &mut performance,
                    )
                };
                context
                    .state
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .summary
                    .record(&result);
                let _ = inspection.response.send(result);
            }
            Message::Shutdown(response) => {
                let _ = response.send(performance);
                break;
            }
        }
    }
}

fn inspect_and_record(
    inspector: &mut impl DecodeInspector,
    request: &DecodeInspectionRequest,
    cancellation: &ScanCancellation,
    context: WorkerContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }
    if read_source_fingerprint_profiled(&request.path, performance)? != request.expected_source {
        return Ok(DecodeInspectionOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }

    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }
    let snapshot = measure_if(
        performance.profiled,
        &mut performance.provider_inspect,
        || inspector.inspect(&request.path),
    )
    .map_err(|message| DecodeInspectionError::Inspector {
        path: request.path.clone(),
        message,
    })?;
    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }
    if snapshot.provider.id != inspector.provider_id()
        || snapshot.provider.version != inspector.provider_version()
    {
        return Err(DecodeInspectionError::Inspector {
            path: request.path.clone(),
            message: format!(
                "inspector provider {}/{} disagrees with snapshot provider {}/{}",
                inspector.provider_id(),
                inspector.provider_version(),
                snapshot.provider.id,
                snapshot.provider.version
            ),
        });
    }

    if read_source_fingerprint_profiled(&request.path, performance)? != request.expected_source {
        return Ok(DecodeInspectionOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }
    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }

    let provider_id = snapshot.provider.id.clone();
    let provider_version = snapshot.provider.version.clone();
    let status = measure_if(
        performance.profiled,
        &mut performance.snapshot_catalog_commit,
        || {
            context
                .catalog
                .record_decode_snapshot(&RecordDecodeSnapshot {
                    representation_id: request.representation_id,
                    expected_source: request.expected_source,
                    snapshot,
                    inspected_at_ms: now_ms(),
                })
        },
    )?;

    Ok(match status {
        RecordDecodeSnapshotStatus::Recorded => {
            let preview = if cancellation.is_cancelled() {
                PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
            } else {
                context
                    .cache
                    .map_or(PreviewCacheOutcome::NotRequested, |cache| {
                        cache_preview(
                            inspector,
                            VisualCacheContext {
                                catalog: context.catalog,
                                cache,
                                technical_observer: context.technical_observer,
                                embedded_preview_sink: context.embedded_preview_sink,
                                request,
                                provider_id: &provider_id,
                                provider_version: &provider_version,
                                cancellation,
                            },
                            context.state,
                            performance,
                        )
                    })
            };
            DecodeInspectionOutcome::Recorded {
                provider_id,
                provider_version,
                preview,
            }
        }
        RecordDecodeSnapshotStatus::StaleSource => {
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::CatalogChanged)
        }
    })
}

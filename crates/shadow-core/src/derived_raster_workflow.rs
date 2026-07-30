//! Staged application workflow for generated raster proposals.
//!
//! Runtime success is not Recipe authority. This owner first verifies and
//! stages the provider bytes, then requires an apply-time generation match
//! before consuming runtime provenance and promoting a durable object.

use std::{fs, path::Path};

use shadow_ai::{
    AiGeneratedPayload, CancellationToken, DerivedRasterPromotionFailure,
    DerivedRasterPromotionRequest, ExecutionLease, GeneratedArtifactReference, LeaseBoundOutput,
    ManagedDerivedRaster, ProviderUnavailable, RuntimeFailure, RuntimeProgressSink,
    RuntimeProvider, RuntimeTerminalOutcome, RuntimeUsage, promote_derived_raster,
};
use thiserror::Error;

use crate::{DerivedRasterStoreError, FilesystemDerivedRasterStore};

/// Runtime-issued proposal authority after the exact provider bytes have been
/// copied into the rebuildable staging namespace.
#[derive(Debug)]
pub struct StagedDerivedRasterProposal {
    promotion_id: String,
    request_id: String,
    generation: u64,
    completed: LeaseBoundOutput<AiGeneratedPayload>,
}

impl StagedDerivedRasterProposal {
    pub fn promotion_id(&self) -> &str {
        &self.promotion_id
    }

    pub fn request_id(&self) -> &str {
        &self.request_id
    }

    pub const fn generation(&self) -> u64 {
        self.generation
    }

    pub const fn payload(&self) -> &AiGeneratedPayload {
        self.completed.payload()
    }
}

/// Application-facing terminal state for one staged provider execution.
#[derive(Debug)]
pub enum DerivedRasterStageOutcome {
    Staged(StagedDerivedRasterProposal),
    Unavailable { reason: ProviderUnavailable },
    Cancelled,
    Failed { failure: RuntimeFailure },
}

#[derive(Debug)]
pub struct DerivedRasterStageReceipt {
    pub request_id: String,
    pub generation: u64,
    pub usage: RuntimeUsage,
    pub outcome: DerivedRasterStageOutcome,
}

/// Executes one admitted provider and stages successful bytes without granting
/// durable Recipe authority.
///
/// The proposal source is request-private scratch owned by this workflow. A
/// successful staging copy retires that source path; cancellation after
/// provider success removes it without promoting an object.
///
/// # Errors
///
/// Returns an error only when runtime succeeded but the proposal store could
/// not verify and stage those exact bytes.
pub fn execute_and_stage_derived_raster<P>(
    store: &FilesystemDerivedRasterStore,
    promotion_id: String,
    lease: ExecutionLease,
    provider: &P,
    cancellation: &CancellationToken,
    proposal_source: &Path,
) -> Result<DerivedRasterStageReceipt, DerivedRasterStageError>
where
    P: RuntimeProvider<Output = AiGeneratedPayload>,
{
    let receipt = lease.execute(provider, cancellation);
    finish_stage_receipt(store, promotion_id, receipt, cancellation, proposal_source)
}

/// Progress-forwarding form of [`execute_and_stage_derived_raster`].
pub fn execute_and_stage_derived_raster_with_progress<P>(
    store: &FilesystemDerivedRasterStore,
    promotion_id: String,
    lease: ExecutionLease,
    provider: &P,
    cancellation: &CancellationToken,
    proposal_source: &Path,
    progress: &dyn RuntimeProgressSink,
) -> Result<DerivedRasterStageReceipt, DerivedRasterStageError>
where
    P: RuntimeProvider<Output = AiGeneratedPayload>,
{
    let receipt = lease.execute_with_progress(provider, cancellation, progress);
    finish_stage_receipt(store, promotion_id, receipt, cancellation, proposal_source)
}

fn finish_stage_receipt(
    store: &FilesystemDerivedRasterStore,
    promotion_id: String,
    receipt: shadow_ai::RuntimeTerminalReceipt<AiGeneratedPayload>,
    cancellation: &CancellationToken,
    proposal_source: &Path,
) -> Result<DerivedRasterStageReceipt, DerivedRasterStageError> {
    let request_id = receipt.request_id;
    let generation = receipt.generation;
    let usage = receipt.usage;
    let outcome = match receipt.outcome {
        RuntimeTerminalOutcome::Succeeded { output } => {
            if cancellation.is_cancelled() {
                remove_request_scratch(proposal_source);
                DerivedRasterStageOutcome::Cancelled
            } else {
                let artifact = payload_artifact(output.payload());
                store.stage_proposal_file(artifact, proposal_source)?;
                remove_request_scratch(proposal_source);
                if cancellation.is_cancelled() {
                    DerivedRasterStageOutcome::Cancelled
                } else {
                    DerivedRasterStageOutcome::Staged(StagedDerivedRasterProposal {
                        promotion_id,
                        request_id: request_id.clone(),
                        generation,
                        completed: *output,
                    })
                }
            }
        }
        RuntimeTerminalOutcome::Unavailable { reason } => {
            DerivedRasterStageOutcome::Unavailable { reason }
        }
        RuntimeTerminalOutcome::Cancelled => DerivedRasterStageOutcome::Cancelled,
        RuntimeTerminalOutcome::Failed { failure } => DerivedRasterStageOutcome::Failed { failure },
    };
    Ok(DerivedRasterStageReceipt {
        request_id,
        generation,
        usage,
        outcome,
    })
}

/// Consumes a staged proposal only if it still matches the caller's current
/// generation, then publishes it into durable managed storage.
///
/// A stale call consumes and drops runtime authority. It never publishes a
/// managed object and cannot be retried against a later generation.
///
/// # Errors
///
/// Returns `StaleGeneration` before durable promotion, or the exact promotion
/// contract/store failure.
pub fn promote_staged_derived_raster_if_current(
    store: &mut FilesystemDerivedRasterStore,
    current_generation: u64,
    staged: StagedDerivedRasterProposal,
) -> Result<ManagedDerivedRaster, CurrentDerivedRasterPromotionFailure> {
    if staged.generation != current_generation {
        return Err(CurrentDerivedRasterPromotionFailure::StaleGeneration {
            proposal_generation: staged.generation,
            current_generation,
        });
    }
    promote_derived_raster(
        store,
        DerivedRasterPromotionRequest::new(staged.promotion_id, staged.completed),
    )
    .map_err(CurrentDerivedRasterPromotionFailure::Promotion)
}

#[derive(Debug, Error)]
pub enum DerivedRasterStageError {
    #[error("failed to verify and stage generated raster proposal bytes")]
    Store(#[from] DerivedRasterStoreError),
}

#[derive(Debug, Error)]
pub enum CurrentDerivedRasterPromotionFailure {
    #[error(
        "generated raster proposal generation {proposal_generation} is stale; current generation is {current_generation}"
    )]
    StaleGeneration {
        proposal_generation: u64,
        current_generation: u64,
    },
    #[error("failed to promote the current generated raster proposal")]
    Promotion(#[source] DerivedRasterPromotionFailure<DerivedRasterStoreError>),
}

fn payload_artifact(payload: &AiGeneratedPayload) -> &GeneratedArtifactReference {
    match payload {
        AiGeneratedPayload::SoftMask(mask) => &mask.artifact,
        AiGeneratedPayload::DenoisedRaster(raster) => &raster.artifact,
    }
}

fn remove_request_scratch(path: &Path) {
    let _ = fs::remove_file(path);
}

#[cfg(test)]
mod tests;

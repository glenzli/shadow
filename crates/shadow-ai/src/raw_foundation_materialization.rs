//! Application transaction for a planned, cached `RawNIND` RAW foundation.
//!
//! Planning resolves the complete source/model/runtime/algorithm cache key
//! before inference. A verified hit returns immediately. A miss owns one
//! store partial through one execution lease and publishes it without
//! overwrite only after both provider and store verification succeed.

use std::path::{Path, PathBuf};

use shadow_cache::{
    FoundationArtifactPublicationStatus, FoundationArtifactStore, FoundationArtifactStoreError,
};
use thiserror::Error;

use crate::{
    AdmittedExecution, AiJobRequest, AiTaskKind, AiTaskParameters, CancellationToken,
    ExecutionLease, InputRole, RawFoundationArtifact, RawNindFoundationPlan,
    RawNindFoundationPlanningError, RawNindFoundationProvider,
    RawNindFoundationProviderConfigurationError, RuntimeContractError, RuntimeProgress,
    RuntimeProgressSink, RuntimeTerminalOutcome, RuntimeTerminalReceipt,
    VerifiedRawNindFoundationInstallation, plan_rawnind_foundation,
    providers::descriptor_from_planned_verification,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RawFoundationMaterializationDisposition {
    ReusedVerified,
    Published,
    ReusedConcurrent,
}

#[derive(Debug)]
pub struct MaterializedRawFoundation {
    disposition: RawFoundationMaterializationDisposition,
    descriptor: RawFoundationArtifact,
    path: PathBuf,
    plan: RawNindFoundationPlan,
    runtime_receipt: Option<RuntimeTerminalReceipt<RawFoundationArtifact>>,
}

impl MaterializedRawFoundation {
    pub const fn disposition(&self) -> RawFoundationMaterializationDisposition {
        self.disposition
    }

    pub const fn descriptor(&self) -> &RawFoundationArtifact {
        &self.descriptor
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub const fn plan(&self) -> &RawNindFoundationPlan {
        &self.plan
    }

    pub const fn runtime_receipt(&self) -> Option<&RuntimeTerminalReceipt<RawFoundationArtifact>> {
        self.runtime_receipt.as_ref()
    }
}

#[derive(Debug)]
pub enum RawFoundationMaterializationOutcome {
    Ready(Box<MaterializedRawFoundation>),
    Terminal(Box<RuntimeTerminalReceipt<RawFoundationArtifact>>),
}

#[derive(Debug, Error)]
pub enum RawFoundationMaterializationError {
    #[error("RAW foundation materialization requires one exact admitted RawFile input")]
    InvalidAdmittedRequest,
    #[error("RAW foundation materialization lease identity is empty")]
    InvalidLeaseIdentity,
    #[error(transparent)]
    Planning(#[from] RawNindFoundationPlanningError),
    #[error(transparent)]
    Store(#[from] FoundationArtifactStoreError),
    #[error(transparent)]
    ProviderConfiguration(#[from] RawNindFoundationProviderConfigurationError),
    #[error(transparent)]
    RuntimeContract(#[from] RuntimeContractError),
    #[error("verified RAW foundation could not produce its portable descriptor")]
    InvalidVerifiedDescriptor,
    #[error("published RAW foundation differs from the runtime-verified descriptor")]
    PublishedDescriptorMismatch,
}

/// Resolves an already materialized foundation without running inference.
///
/// Planning still verifies the admitted model/runtime, decodes the exact RAW,
/// and recomputes its deterministic preprocessing/cache contract. A verified
/// managed-store hit is returned with the same portable descriptor as normal
/// materialization; a clean miss returns `None`.
///
/// # Errors
///
/// Returns an error for planning, source/model substitution, cache integrity,
/// or descriptor identity failure.
pub fn resolve_cached_rawnind_foundation(
    store: &FoundationArtifactStore,
    installation: &VerifiedRawNindFoundationInstallation,
    input_raw: impl Into<PathBuf>,
    expected_source_sha256: &str,
    expected_source_size_bytes: u64,
    cancellation: &CancellationToken,
) -> Result<Option<MaterializedRawFoundation>, RawFoundationMaterializationError> {
    let (_, cached) = plan_and_lookup_cached_foundation(
        store,
        installation,
        input_raw.into(),
        expected_source_sha256,
        expected_source_size_bytes,
        cancellation,
    )?;
    Ok(cached)
}

/// Materializes one foundation while discarding intermediate progress.
///
/// A cache hit does not issue or consume an execution lease. A cache miss
/// returns the complete runtime terminal receipt whether the provider
/// succeeds, fails, becomes unavailable, or is cancelled.
///
/// # Errors
///
/// Returns an error for planning, cache integrity, provider configuration,
/// lease construction, publication, or descriptor identity failures.
pub fn materialize_rawnind_foundation(
    store: &FoundationArtifactStore,
    installation: &VerifiedRawNindFoundationInstallation,
    lease_id: String,
    execution: AdmittedExecution,
    input_raw: impl Into<PathBuf>,
    cancellation: &CancellationToken,
) -> Result<RawFoundationMaterializationOutcome, RawFoundationMaterializationError> {
    materialize_rawnind_foundation_with_progress(
        store,
        installation,
        lease_id,
        execution,
        input_raw,
        cancellation,
        &DiscardMaterializationProgress,
    )
}

/// Materializes one foundation and forwards monotonic provider progress.
///
/// # Errors
///
/// Returns the same fail-closed errors as
/// [`materialize_rawnind_foundation`].
pub fn materialize_rawnind_foundation_with_progress(
    store: &FoundationArtifactStore,
    installation: &VerifiedRawNindFoundationInstallation,
    lease_id: String,
    execution: AdmittedExecution,
    input_raw: impl Into<PathBuf>,
    cancellation: &CancellationToken,
    progress_sink: &dyn RuntimeProgressSink,
) -> Result<RawFoundationMaterializationOutcome, RawFoundationMaterializationError> {
    if lease_id.trim().is_empty() {
        return Err(RawFoundationMaterializationError::InvalidLeaseIdentity);
    }
    let input = exact_raw_foundation_input(execution.request())
        .ok_or(RawFoundationMaterializationError::InvalidAdmittedRequest)?;
    let source_sha256 = input.content_hash.clone();
    let source_size_bytes = input.byte_len;
    let input_raw = input_raw.into();
    let (plan, cached) = plan_and_lookup_cached_foundation(
        store,
        installation,
        input_raw.clone(),
        &source_sha256,
        source_size_bytes,
        cancellation,
    )?;
    if let Some(cached) = cached {
        return Ok(RawFoundationMaterializationOutcome::Ready(Box::new(cached)));
    }

    let partial = store.allocate_partial_path(plan.cache_key_sha256())?;
    let execution_plan = execution.plan_identity().clone();
    let provider = RawNindFoundationProvider::new(
        installation,
        execution_plan,
        plan.clone(),
        input_raw,
        &partial,
    )?;
    let lease = ExecutionLease::issue(lease_id, execution)?;
    let runtime_receipt = lease.execute_with_progress(&provider, cancellation, progress_sink);
    let runtime_descriptor = match &runtime_receipt.outcome {
        RuntimeTerminalOutcome::Succeeded { output } => output.payload().clone(),
        RuntimeTerminalOutcome::Unavailable { .. }
        | RuntimeTerminalOutcome::Cancelled
        | RuntimeTerminalOutcome::Failed { .. } => {
            store.discard_partial(&partial)?;
            return Ok(RawFoundationMaterializationOutcome::Terminal(Box::new(
                runtime_receipt,
            )));
        }
    };

    let publication = store.publish_verified_partial(plan.cache_key_sha256(), &partial)?;
    let descriptor = descriptor_from_planned_verification(publication.verification, &plan)
        .map_err(|()| RawFoundationMaterializationError::InvalidVerifiedDescriptor)?;
    if descriptor != runtime_descriptor {
        return Err(RawFoundationMaterializationError::PublishedDescriptorMismatch);
    }
    let disposition = publication_disposition(publication.status);
    Ok(RawFoundationMaterializationOutcome::Ready(Box::new(
        MaterializedRawFoundation {
            disposition,
            descriptor,
            path: publication.path,
            plan,
            runtime_receipt: Some(runtime_receipt),
        },
    )))
}

fn plan_and_lookup_cached_foundation(
    store: &FoundationArtifactStore,
    installation: &VerifiedRawNindFoundationInstallation,
    input_raw: PathBuf,
    expected_source_sha256: &str,
    expected_source_size_bytes: u64,
    cancellation: &CancellationToken,
) -> Result<
    (RawNindFoundationPlan, Option<MaterializedRawFoundation>),
    RawFoundationMaterializationError,
> {
    let plan = plan_rawnind_foundation(
        installation,
        input_raw,
        expected_source_sha256,
        expected_source_size_bytes,
        cancellation,
    )?;
    let Some(reader) = store.lookup_verified(plan.cache_key_sha256())? else {
        return Ok((plan, None));
    };
    let verification = reader.verification().clone();
    drop(reader);
    let path = verification.path.clone();
    let descriptor = descriptor_from_planned_verification(verification, &plan)
        .map_err(|()| RawFoundationMaterializationError::InvalidVerifiedDescriptor)?;
    Ok((
        plan.clone(),
        Some(MaterializedRawFoundation {
            disposition: RawFoundationMaterializationDisposition::ReusedVerified,
            descriptor,
            path,
            plan,
            runtime_receipt: None,
        }),
    ))
}

fn exact_raw_foundation_input(request: &AiJobRequest) -> Option<&crate::ArtifactReference> {
    if request.task != AiTaskKind::MaterializeRawFoundation
        || request.parameters != AiTaskParameters::RawFoundation
        || request.inputs.len() != 1
    {
        return None;
    }
    let input = &request.inputs[0];
    (input.role == InputRole::RawFile).then_some(input)
}

const fn publication_disposition(
    status: FoundationArtifactPublicationStatus,
) -> RawFoundationMaterializationDisposition {
    match status {
        FoundationArtifactPublicationStatus::Published => {
            RawFoundationMaterializationDisposition::Published
        }
        FoundationArtifactPublicationStatus::ReusedExisting => {
            RawFoundationMaterializationDisposition::ReusedConcurrent
        }
    }
}

#[derive(Debug)]
struct DiscardMaterializationProgress;

impl RuntimeProgressSink for DiscardMaterializationProgress {
    fn publish(&self, _progress: RuntimeProgress) {}
}

#[cfg(test)]
mod tests;

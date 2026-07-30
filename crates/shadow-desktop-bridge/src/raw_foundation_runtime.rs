//! Desktop application adapter for one admitted `RawNIND` foundation.
//!
//! The source path is converted into a complete SHA-256 inventory before the
//! provider is admitted. Model verification, CPU resource admission, planned
//! cache lookup, one-shot execution, and verified publication then form one
//! serialized local transaction. The resulting cache path is transient
//! application state and never becomes part of a Recipe.

pub(crate) mod config;

use std::{
    path::PathBuf,
    process::Command,
    str::FromStr,
    sync::{Mutex, MutexGuard},
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactReference,
    BackendKind, CancellationToken, ExecutionBackend, FallbackDisclosure, HardwareProfile,
    InputRole, LocalExecutionAdmission, LocalExecutionBinding, LocalModelAvailability,
    MaterializedRawFoundation, ModelAvailability, NumericPrecision, ObservationTarget,
    OnBatteryPolicy, PrivacyClass, RawFoundationArtifact, RawFoundationMaterializationDisposition,
    RawFoundationMaterializationOutcome, RawNindFoundationInput, ResourceEstimate, ResourcePolicy,
    RuntimeFailure, RuntimeProgressSink, RuntimeTerminalOutcome, TaskPriority,
    VerifiedRawNindFoundationInstallation, admit_local_execution,
    materialize_rawnind_foundation_with_progress, resolve_cached_rawnind_foundation,
    verify_rawnind_foundation_installation,
};
use shadow_cache::{
    FoundationArtifactError, FoundationArtifactStore, FoundationArtifactStoreError, sha256_file,
};
use shadow_catalog::RepresentationFingerprint;
use shadow_core::fingerprint_source;
use shadow_domain::{PhotoId, Platform};
use thiserror::Error;

use self::config::RawFoundationRuntimePaths;
use crate::isolated_proxy::{
    IsolatedRawFrameStaging, configured_helper_path, stage_isolated_raw_frame,
};

const MEBIBYTE: u64 = 1024 * 1024;
const GIBIBYTE: u64 = 1024 * MEBIBYTE;

#[derive(Debug)]
pub(crate) struct RawFoundationRuntime {
    provider_executable: PathBuf,
    model_package: PathBuf,
    model_graph: PathBuf,
    manifest_path: PathBuf,
    raw_frame_staging_root: PathBuf,
    store: FoundationArtifactStore,
    installation: Mutex<Option<VerifiedRawNindFoundationInstallation>>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationInvocation {
    pub(crate) request_id: String,
    pub(crate) generation: u64,
    pub(crate) photo_id: String,
    pub(crate) input_raw: PathBuf,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationRuntimeAvailability {
    pub(crate) model_id: String,
    pub(crate) runtime_version: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationReady {
    pub(crate) descriptor: RawFoundationArtifact,
    pub(crate) path: PathBuf,
    pub(crate) source_path: PathBuf,
    pub(crate) source: RepresentationFingerprint,
    pub(crate) disposition: RawFoundationMaterializationDisposition,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) enum RawFoundationRuntimeOutcome {
    Ready(Box<RawFoundationReady>),
    Unavailable { diagnostic: String },
    Cancelled,
    Failed { diagnostic: String },
}

#[derive(Debug, Clone)]
struct RawFoundationRuntimeResources {
    hardware: HardwareProfile,
    policy: ResourcePolicy,
    route_estimate: ResourceEstimate,
}

impl RawFoundationRuntime {
    pub(crate) fn open(
        paths: RawFoundationRuntimePaths,
    ) -> Result<Self, RawFoundationRuntimeError> {
        Ok(Self {
            provider_executable: paths.provider_executable,
            model_package: paths.model_package,
            model_graph: paths.model_graph,
            manifest_path: paths.manifest_path,
            raw_frame_staging_root: paths.raw_frame_staging_root,
            store: FoundationArtifactStore::open(paths.foundation_store_root)?,
            installation: Mutex::new(None),
        })
    }

    /// Verifies the exact installed model and runtime without planning a RAW.
    pub(crate) fn probe(
        &self,
        cancellation: &CancellationToken,
    ) -> Result<RawFoundationRuntimeAvailability, RawFoundationRuntimeError> {
        let mut installation = self.installation_guard()?;
        let installation = self.verified_installation(&mut installation, cancellation)?;
        Ok(RawFoundationRuntimeAvailability {
            model_id: installation.manifest().model_id.clone(),
            runtime_version: installation.runtime_version().to_owned(),
        })
    }

    pub(crate) fn materialize(
        &self,
        invocation: &RawFoundationInvocation,
        cancellation: &CancellationToken,
        progress: &dyn RuntimeProgressSink,
    ) -> Result<RawFoundationRuntimeOutcome, RawFoundationRuntimeError> {
        if cancellation.is_cancelled() {
            return Ok(RawFoundationRuntimeOutcome::Cancelled);
        }
        let photo_id = PhotoId::from_str(&invocation.photo_id)
            .map_err(|_| RawFoundationRuntimeError::InvalidPhotoId)?;
        let before = fingerprint_source(&invocation.input_raw)
            .map_err(RawFoundationRuntimeError::SourceInventory)?;
        let source_sha256 = sha256_file(&invocation.input_raw)?;
        let after_hash = fingerprint_source(&invocation.input_raw)
            .map_err(RawFoundationRuntimeError::SourceInventory)?;
        if before != after_hash {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        if cancellation.is_cancelled() {
            return Ok(RawFoundationRuntimeOutcome::Cancelled);
        }

        let mut installation_guard = self.installation_guard()?;
        let installation = self.verified_installation(&mut installation_guard, cancellation)?;
        let resources = RawFoundationRuntimeResources::current_macos(installation.backend_id())?;
        let request = foundation_request(
            invocation,
            photo_id,
            source_sha256,
            before.byte_len,
            resources.route_estimate,
        );
        let admission = admit_local_execution(
            LocalExecutionBinding {
                execution_id: format!("raw-foundation-execution-{}", invocation.request_id),
                request,
                provider: installation.provider_identity().clone(),
                route_estimate: resources.route_estimate,
                fallback: FallbackDisclosure::Primary,
            },
            installation.manifest(),
            &resources.hardware,
            resources.policy,
            ModelAvailability {
                local: LocalModelAvailability::Installed {
                    artifact_set_blake3: installation
                        .manifest()
                        .artifact_set
                        .inventory_blake3
                        .clone(),
                    license_accepted: true,
                },
            },
        )?;
        let LocalExecutionAdmission::Admitted { execution } = admission else {
            let LocalExecutionAdmission::Deferred { blockers } = admission else {
                unreachable!("local admission has exactly two variants");
            };
            return Err(RawFoundationRuntimeError::AdmissionDeferred(format!(
                "{blockers:?}"
            )));
        };
        let staging = self.stage_decoded_raw_frame(&invocation.input_raw)?;
        let provider_input = staging.as_ref().map_or_else(
            || RawNindFoundationInput::from(&invocation.input_raw),
            |staging| {
                RawNindFoundationInput::with_decoded_raw_frame(
                    &invocation.input_raw,
                    staging.manifest_path(),
                )
            },
        );
        let outcome = materialize_rawnind_foundation_with_progress(
            &self.store,
            installation,
            format!("raw-foundation-lease-{}", invocation.request_id),
            *execution,
            provider_input,
            cancellation,
            progress,
        )?;
        drop(installation_guard);

        if fingerprint_source(&invocation.input_raw)
            .map_err(RawFoundationRuntimeError::SourceInventory)?
            != before
        {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        Ok(runtime_outcome(outcome, &invocation.input_raw, before))
    }

    /// Recomputes the exact plan and resolves only an existing verified cache hit.
    ///
    /// This supports persistent Recipe intent after process restart. It may
    /// decode/plan the RAW in the sidecar, but it never admits or runs model
    /// inference and never creates a cache partial.
    pub(crate) fn resolve_cached(
        &self,
        input_raw: &std::path::Path,
        cancellation: &CancellationToken,
    ) -> Result<Option<RawFoundationReady>, RawFoundationRuntimeError> {
        if cancellation.is_cancelled() {
            return Err(RawFoundationRuntimeError::Cancelled);
        }
        let before =
            fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?;
        let source_sha256 = sha256_file(input_raw)?;
        let after_hash =
            fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?;
        if before != after_hash {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        let mut installation_guard = self.installation_guard()?;
        let installation = self.verified_installation(&mut installation_guard, cancellation)?;
        let staging = self.stage_decoded_raw_frame(input_raw)?;
        let provider_input = staging.as_ref().map_or_else(
            || RawNindFoundationInput::from(input_raw),
            |staging| {
                RawNindFoundationInput::with_decoded_raw_frame(input_raw, staging.manifest_path())
            },
        );
        let cached = resolve_cached_rawnind_foundation(
            &self.store,
            installation,
            provider_input,
            &source_sha256,
            before.byte_len,
            cancellation,
        )?;
        drop(installation_guard);
        if fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?
            != before
        {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        Ok(cached.map(|materialized| ready_from_materialized(&materialized, input_raw, before)))
    }

    fn stage_decoded_raw_frame(
        &self,
        input_raw: &std::path::Path,
    ) -> Result<Option<IsolatedRawFrameStaging>, RawFoundationRuntimeError> {
        let Some(helper_path) = configured_helper_path() else {
            return Ok(None);
        };
        stage_isolated_raw_frame(&helper_path, &self.raw_frame_staging_root, input_raw)
            .map(Some)
            .map_err(|error| RawFoundationRuntimeError::DecodedInput(error.to_string()))
    }

    fn installation_guard(
        &self,
    ) -> Result<
        MutexGuard<'_, Option<VerifiedRawNindFoundationInstallation>>,
        RawFoundationRuntimeError,
    > {
        self.installation
            .lock()
            .map_err(|_| RawFoundationRuntimeError::StatePoisoned)
    }

    fn verified_installation<'installation>(
        &self,
        guard: &'installation mut Option<VerifiedRawNindFoundationInstallation>,
        cancellation: &CancellationToken,
    ) -> Result<&'installation VerifiedRawNindFoundationInstallation, RawFoundationRuntimeError>
    {
        if guard.is_none() {
            *guard = Some(verify_rawnind_foundation_installation(
                &self.provider_executable,
                &self.model_package,
                &self.model_graph,
                &self.manifest_path,
                cancellation,
            )?);
        }
        Ok(guard
            .as_ref()
            .expect("verified RAW foundation installation was inserted"))
    }
}

fn foundation_request(
    invocation: &RawFoundationInvocation,
    photo_id: PhotoId,
    source_sha256: String,
    source_size_bytes: u64,
    estimate: ResourceEstimate,
) -> AiJobRequest {
    AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: invocation.request_id.clone(),
        generation: invocation.generation,
        task: AiTaskKind::MaterializeRawFoundation,
        target: ObservationTarget::Photo { photo_id },
        priority: TaskPriority::CurrentInput,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::RawFile,
            content_hash: source_sha256,
            byte_len: source_size_bytes,
            media_type: "image/x-raw".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: AiTaskParameters::RawFoundation,
        estimate,
    }
}

fn runtime_outcome(
    outcome: RawFoundationMaterializationOutcome,
    source_path: &std::path::Path,
    source: RepresentationFingerprint,
) -> RawFoundationRuntimeOutcome {
    match outcome {
        RawFoundationMaterializationOutcome::Ready(materialized) => {
            RawFoundationRuntimeOutcome::Ready(Box::new(ready_from_materialized(
                &materialized,
                source_path,
                source,
            )))
        }
        RawFoundationMaterializationOutcome::Terminal(receipt) => match receipt.outcome {
            RuntimeTerminalOutcome::Unavailable { reason } => {
                RawFoundationRuntimeOutcome::Unavailable {
                    diagnostic: format!("{reason:?}"),
                }
            }
            RuntimeTerminalOutcome::Cancelled => RawFoundationRuntimeOutcome::Cancelled,
            RuntimeTerminalOutcome::Failed { failure } => RawFoundationRuntimeOutcome::Failed {
                diagnostic: runtime_failure_diagnostic(&failure),
            },
            RuntimeTerminalOutcome::Succeeded { .. } => RawFoundationRuntimeOutcome::Failed {
                diagnostic: "materialization returned an unpublished successful receipt".into(),
            },
        },
    }
}

fn ready_from_materialized(
    materialized: &MaterializedRawFoundation,
    source_path: &std::path::Path,
    source: RepresentationFingerprint,
) -> RawFoundationReady {
    RawFoundationReady {
        descriptor: materialized.descriptor().clone(),
        path: materialized.path().to_path_buf(),
        source_path: source_path.to_path_buf(),
        source,
        disposition: materialized.disposition(),
    }
}

fn runtime_failure_diagnostic(failure: &RuntimeFailure) -> String {
    format!("{failure:?}")
}

impl RawFoundationRuntimeResources {
    fn current_macos(backend_id: &str) -> Result<Self, RawFoundationRuntimeError> {
        if !cfg!(target_os = "macos") {
            return Err(RawFoundationRuntimeError::PlatformUnsupported);
        }
        let total_system_ram_bytes = macos_total_memory_bytes()?;
        let logical_cpu_threads = u32::try_from(
            std::thread::available_parallelism()
                .map_err(|_| RawFoundationRuntimeError::HardwareUnavailable)?
                .get(),
        )
        .map_err(|_| RawFoundationRuntimeError::HardwareUnavailable)?;
        let available_system_ram_bytes = total_system_ram_bytes / 2;
        let maximum_ai_system_ram_bytes = total_system_ram_bytes / 2;
        let reserved_system_ram_bytes = (total_system_ram_bytes / 8).min(GIBIBYTE);
        let cpu_threads = logical_cpu_threads.clamp(1, 4);
        let route_estimate = ResourceEstimate {
            peak_system_ram_bytes: 2 * GIBIBYTE,
            peak_device_memory_bytes: 0,
            cpu_threads,
            scratch_disk_bytes: 2 * GIBIBYTE,
            upload_bytes: 0,
            estimated_duration_ms: Some(180_000),
        };
        Ok(Self {
            hardware: HardwareProfile {
                platform: Platform::MacOs,
                total_system_ram_bytes,
                available_system_ram_bytes,
                logical_cpu_threads,
                on_battery: false,
                backends: vec![ExecutionBackend {
                    id: backend_id.into(),
                    kind: BackendKind::Cpu,
                    available: true,
                    supported_precisions: vec![NumericPrecision::Float32],
                    total_device_memory_bytes: None,
                    available_device_memory_bytes: None,
                    maximum_concurrent_sessions: 1,
                    active_sessions: 0,
                }],
            },
            policy: ResourcePolicy {
                maximum_ai_system_ram_bytes,
                reserved_system_ram_bytes,
                maximum_ai_cpu_threads: logical_cpu_threads.clamp(1, 4),
                maximum_device_memory_percent: 75,
                on_battery: OnBatteryPolicy::Continue,
            },
            route_estimate,
        })
    }
}

fn macos_total_memory_bytes() -> Result<u64, RawFoundationRuntimeError> {
    let output = Command::new("/usr/sbin/sysctl")
        .args(["-n", "hw.memsize"])
        .output()
        .map_err(|_| RawFoundationRuntimeError::HardwareUnavailable)?;
    if !output.status.success() {
        return Err(RawFoundationRuntimeError::HardwareUnavailable);
    }
    std::str::from_utf8(&output.stdout)
        .ok()
        .and_then(|value| value.trim().parse().ok())
        .filter(|value| *value > 0)
        .ok_or(RawFoundationRuntimeError::HardwareUnavailable)
}

#[derive(Debug, Error)]
pub(crate) enum RawFoundationRuntimeError {
    #[error("RAW foundation runtime supports only macOS")]
    PlatformUnsupported,
    #[error("RAW foundation hardware capacity is unavailable")]
    HardwareUnavailable,
    #[error("RAW foundation photo identity is invalid")]
    InvalidPhotoId,
    #[error("RAW foundation source could not be inventoried")]
    SourceInventory(#[source] std::io::Error),
    #[error("RAW foundation source changed during materialization")]
    SourceChanged,
    #[error("RAW foundation could not stage the decoded Bayer input: {0}")]
    DecodedInput(String),
    #[error("RAW foundation cache resolution was cancelled")]
    Cancelled,
    #[error("RAW foundation runtime state is poisoned")]
    StatePoisoned,
    #[error("RAW foundation resource admission was deferred: {0}")]
    AdmissionDeferred(String),
    #[error(transparent)]
    SourceHash(#[from] FoundationArtifactError),
    #[error(transparent)]
    Store(#[from] FoundationArtifactStoreError),
    #[error(transparent)]
    Verification(#[from] shadow_ai::RawNindFoundationModelVerificationError),
    #[error(transparent)]
    RuntimeContract(#[from] shadow_ai::RuntimeContractError),
    #[error(transparent)]
    Materialization(#[from] shadow_ai::RawFoundationMaterializationError),
}

#[cfg(test)]
mod tests;

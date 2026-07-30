//! Fail-closed local-process adapter for the public `RawNIND` Bayer foundation.
//!
//! The executable, public release package, extracted ONNX graph, manifest, RAW
//! input, and output all remain explicit. The provider accepts no generic
//! denoise task and returns a [`crate::RawFoundationArtifact`] only after the
//! complete `.shadowrawf` file passes Shadow's independent verifier.

use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use shadow_cache::{FoundationArtifactVerification, verify_foundation_artifact};
use thiserror::Error;

use crate::{
    AdmittedExecution, AdmittedModelIdentity, AiCapability, AiTaskKind, AiTaskParameters,
    ArtifactHashAlgorithm, BackendKind, CancellationToken, ExecutionPlanIdentity,
    ExecutionRouteIdentity, GeneratedArtifactReference, InputRole, ModelFormat, ModelManifest,
    NumericPrecision, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    ProviderUnavailable, RAW_FOUNDATION_ENCODING_VERSION, RAW_FOUNDATION_MEDIA_TYPE, RasterExtent,
    RawFoundationArtifact, RawFoundationProvenance, RawFoundationSourceProvenance,
    RuntimeContractError, RuntimeFailure, RuntimeProgress, RuntimeProgressReporter,
    RuntimeProvider, RuntimeUsage,
};

use super::local_process::{ProcessFailure, wait_with_bounded_output};

pub const RAWNIND_FOUNDATION_PROVIDER_ID: &str = "shadow.rawnind.foundation-sidecar";
pub const RAWNIND_FOUNDATION_ADAPTER_REVISION: &str =
    "rawnind-foundation-sidecar-protocol-20260731.1";
pub const RAWNIND_FOUNDATION_MODEL_ID: &str = "darktable-ai/rawnind-public-bayer";
pub const RAWNIND_FOUNDATION_MODEL_REVISION: &str =
    "release-5.6.0@5454d7aa6d89a67054fd4a83343b09e69acaf76a";
pub const RAWNIND_FOUNDATION_ARTIFACT_SET_BLAKE3: &str =
    "51bfabe88964e78ac74007e7b499e61ed07dc9dba51c28f3d0d495becb611d94";
pub const RAWNIND_FOUNDATION_PACKAGE_SHA256: &str =
    "d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af";
pub const RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256: &str =
    "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901";
pub const RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256: &str =
    "e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f";
pub const RAWNIND_FOUNDATION_IMPLEMENTATION_REVISION: &str =
    "rawnind-public-bayer-foundation-20260731.1";
pub const RAWNIND_FOUNDATION_MODEL_RECEIPT_PREFIX: &str = "shadow-rawnind-foundation-model-v1";
pub const RAWNIND_FOUNDATION_PLAN_RECEIPT_PREFIX: &str = "shadow-rawnind-foundation-plan-v1";
pub const RAWNIND_FOUNDATION_RECEIPT_PREFIX: &str = "shadow-rawnind-foundation-v1";

const POLL_INTERVAL: Duration = Duration::from_millis(10);
const CHECKED_IN_MANIFEST: &[u8] =
    include_bytes!("../../../../apps/desktop/providers/rawnind-foundation/model-manifest.json");

/// Installation authority issued only after the sidecar verifies both public
/// release identities and reports its concrete ONNX Runtime revision.
#[derive(Debug)]
pub struct VerifiedRawNindFoundationInstallation {
    manifest: ModelManifest,
    model_identity: AdmittedModelIdentity,
    provider_identity: ProviderIdentity,
    executable: PathBuf,
    model_package: PathBuf,
    model_graph: PathBuf,
    manifest_path: PathBuf,
    runtime_version: String,
    backend_id: String,
}

/// Exact no-inference cache plan for one source RAW and admitted runtime.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RawNindFoundationPlan {
    cache_key_sha256: String,
    source_sha256: String,
    source_size_bytes: u64,
    source_pixel_contract_sha256: String,
    raster_extent: RasterExtent,
    runtime_version: String,
}

impl RawNindFoundationPlan {
    pub fn cache_key_sha256(&self) -> &str {
        &self.cache_key_sha256
    }

    pub fn source_sha256(&self) -> &str {
        &self.source_sha256
    }

    pub const fn source_size_bytes(&self) -> u64 {
        self.source_size_bytes
    }

    pub fn source_pixel_contract_sha256(&self) -> &str {
        &self.source_pixel_contract_sha256
    }

    pub const fn raster_extent(&self) -> RasterExtent {
        self.raster_extent
    }

    pub fn runtime_version(&self) -> &str {
        &self.runtime_version
    }
}

impl VerifiedRawNindFoundationInstallation {
    pub const fn manifest(&self) -> &ModelManifest {
        &self.manifest
    }

    pub const fn model_identity(&self) -> &AdmittedModelIdentity {
        &self.model_identity
    }

    pub const fn provider_identity(&self) -> &ProviderIdentity {
        &self.provider_identity
    }

    pub fn executable(&self) -> &Path {
        &self.executable
    }

    pub fn model_package(&self) -> &Path {
        &self.model_package
    }

    pub fn model_graph(&self) -> &Path {
        &self.model_graph
    }

    pub fn manifest_path(&self) -> &Path {
        &self.manifest_path
    }

    pub fn runtime_version(&self) -> &str {
        &self.runtime_version
    }

    pub fn backend_id(&self) -> &str {
        &self.backend_id
    }
}

/// Computes the complete v2 cache identity without running a model tile.
///
/// The sidecar still verifies and loads the exact admitted model/runtime,
/// decodes the RAW, and derives the deterministic preprocessing/stripe
/// contract. The expected source identity comes from the caller's already
/// content-addressed input inventory and prevents planning different bytes.
///
/// # Errors
///
/// Returns an error for an absent or changed source, cancellation, process
/// failure, or any malformed/substituted plan receipt.
pub fn plan_rawnind_foundation(
    installation: &VerifiedRawNindFoundationInstallation,
    input_raw: impl Into<PathBuf>,
    expected_source_sha256: &str,
    expected_source_size_bytes: u64,
    cancellation: &CancellationToken,
) -> Result<RawNindFoundationPlan, RawNindFoundationPlanningError> {
    validate_lower_sha256(expected_source_sha256)
        .map_err(|()| RawNindFoundationPlanningError::InvalidSourceIdentity)?;
    if expected_source_size_bytes == 0 {
        return Err(RawNindFoundationPlanningError::InvalidSourceIdentity);
    }
    let input_raw = input_raw.into();
    let metadata =
        fs::metadata(&input_raw).map_err(|_| RawNindFoundationPlanningError::SourceUnavailable)?;
    if !metadata.is_file() || metadata.len() != expected_source_size_bytes {
        return Err(RawNindFoundationPlanningError::SourceIdentityChanged);
    }
    if cancellation.is_cancelled() {
        return Err(RawNindFoundationPlanningError::Cancelled);
    }

    let child = Command::new(&installation.executable)
        .arg("--model-package")
        .arg(&installation.model_package)
        .arg("--model-graph")
        .arg(&installation.model_graph)
        .arg("--manifest")
        .arg(&installation.manifest_path)
        .arg("--input-raw")
        .arg(&input_raw)
        .arg("--source-pixel-contract-sha256")
        .arg(RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256)
        .arg("--plan")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|_| RawNindFoundationPlanningError::PlanningFailed)?;
    let captured =
        wait_with_bounded_output(child, cancellation, POLL_INTERVAL).map_err(|failure| {
            match failure {
                ProcessFailure::Cancelled => RawNindFoundationPlanningError::Cancelled,
                ProcessFailure::Wait | ProcessFailure::Reader => {
                    RawNindFoundationPlanningError::PlanningFailed
                }
            }
        })?;
    if !captured.status.success() {
        return Err(RawNindFoundationPlanningError::PlanningFailed);
    }
    let plan = parse_plan_receipt(&captured.stdout)
        .map_err(|()| RawNindFoundationPlanningError::PlanningFailed)?;
    if plan.source_sha256 != expected_source_sha256
        || plan.source_size_bytes != expected_source_size_bytes
        || plan.source_pixel_contract_sha256 != RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256
        || plan.runtime_version != installation.runtime_version
    {
        return Err(RawNindFoundationPlanningError::SourceIdentityChanged);
    }
    Ok(plan)
}

/// Verifies a side-loaded `RawNIND` installation before resource admission.
///
/// The executable owns archive and ONNX graph hashing because those same bytes
/// are what it will load. Rust independently checks its bounded receipt and the
/// complete checked-in manifest before issuing installation authority.
///
/// # Errors
///
/// Returns an error for absent bytes, a changed manifest, cancellation, an
/// invalid sidecar receipt, or a failed native verification.
pub fn verify_rawnind_foundation_installation(
    executable: impl Into<PathBuf>,
    model_package: impl Into<PathBuf>,
    model_graph: impl Into<PathBuf>,
    manifest_path: impl Into<PathBuf>,
    cancellation: &CancellationToken,
) -> Result<VerifiedRawNindFoundationInstallation, RawNindFoundationModelVerificationError> {
    let executable = executable.into();
    let model_package = model_package.into();
    let model_graph = model_graph.into();
    let manifest_path = manifest_path.into();
    if !executable.is_file() {
        return Err(RawNindFoundationModelVerificationError::AdapterNotLinked);
    }
    if !model_package.is_file() || !model_graph.is_file() {
        return Err(RawNindFoundationModelVerificationError::ModelNotInstalled);
    }
    let manifest = load_exact_manifest(&manifest_path)?;
    if cancellation.is_cancelled() {
        return Err(RawNindFoundationModelVerificationError::Cancelled);
    }

    let child = Command::new(&executable)
        .arg("--model-package")
        .arg(&model_package)
        .arg("--model-graph")
        .arg(&model_graph)
        .arg("--manifest")
        .arg(&manifest_path)
        .arg("--verify-model")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|_| RawNindFoundationModelVerificationError::VerificationFailed)?;
    let captured =
        wait_with_bounded_output(child, cancellation, POLL_INTERVAL).map_err(|failure| {
            match failure {
                ProcessFailure::Cancelled => RawNindFoundationModelVerificationError::Cancelled,
                ProcessFailure::Wait | ProcessFailure::Reader => {
                    RawNindFoundationModelVerificationError::VerificationFailed
                }
            }
        })?;
    if !captured.status.success() {
        return Err(RawNindFoundationModelVerificationError::VerificationFailed);
    }
    let runtime_version = parse_model_receipt(&captured.stdout)
        .map_err(|()| RawNindFoundationModelVerificationError::VerificationFailed)?;
    let provider_identity = ProviderIdentity {
        provider_id: RAWNIND_FOUNDATION_PROVIDER_ID.into(),
        adapter_revision: format!(
            "{RAWNIND_FOUNDATION_ADAPTER_REVISION}+onnxruntime-{runtime_version}"
        ),
        execution_class: ProviderExecutionClass::LocalModel,
    };
    let backend_id = format!("onnxruntime-cpu:{runtime_version}");
    Ok(VerifiedRawNindFoundationInstallation {
        model_identity: model_identity(&manifest),
        provider_identity,
        manifest,
        executable,
        model_package,
        model_graph,
        manifest_path,
        runtime_version,
        backend_id,
    })
}

trait FoundationOutputVerifier: std::fmt::Debug + Send + Sync {
    fn verify(
        &self,
        path: &Path,
    ) -> Result<FoundationArtifactVerification, RawNindFoundationOutputVerificationError>;
}

#[derive(Debug)]
struct IndependentFoundationOutputVerifier;

impl FoundationOutputVerifier for IndependentFoundationOutputVerifier {
    fn verify(
        &self,
        path: &Path,
    ) -> Result<FoundationArtifactVerification, RawNindFoundationOutputVerificationError> {
        verify_foundation_artifact(path)
            .map_err(|_| RawNindFoundationOutputVerificationError::InvalidArtifact)
    }
}

/// One request-scoped, single-use `RawNIND` foundation provider.
#[derive(Debug)]
pub struct RawNindFoundationProvider {
    route: ExecutionRouteIdentity,
    execution_plan: ExecutionPlanIdentity,
    foundation_plan: RawNindFoundationPlan,
    executable: PathBuf,
    model_package: PathBuf,
    model_graph: PathBuf,
    manifest_path: PathBuf,
    runtime_version: String,
    input_raw: PathBuf,
    output_foundation: PathBuf,
    output_verifier: Arc<dyn FoundationOutputVerifier>,
    execution_started: Mutex<bool>,
}

impl RawNindFoundationProvider {
    /// Constructs a request-scoped provider around one exact admitted model.
    ///
    /// The destination must be an absent partial allocated by the foundation
    /// store. Publication into the stable cache happens only after the runtime
    /// lease succeeds.
    ///
    /// # Errors
    ///
    /// Returns an error for an incompatible plan, malformed source identity,
    /// path conflict, or invalid runtime route.
    pub fn new(
        installation: &VerifiedRawNindFoundationInstallation,
        execution_plan: ExecutionPlanIdentity,
        foundation_plan: RawNindFoundationPlan,
        input_raw: impl Into<PathBuf>,
        output_foundation: impl Into<PathBuf>,
    ) -> Result<Self, RawNindFoundationProviderConfigurationError> {
        Self::new_with_verifier(
            installation,
            execution_plan,
            foundation_plan,
            input_raw.into(),
            output_foundation.into(),
            Arc::new(IndependentFoundationOutputVerifier),
        )
    }

    fn new_with_verifier(
        installation: &VerifiedRawNindFoundationInstallation,
        execution_plan: ExecutionPlanIdentity,
        foundation_plan: RawNindFoundationPlan,
        input_raw: PathBuf,
        output_foundation: PathBuf,
        output_verifier: Arc<dyn FoundationOutputVerifier>,
    ) -> Result<Self, RawNindFoundationProviderConfigurationError> {
        execution_plan.validate()?;
        if execution_plan.plan.backend_kind != BackendKind::Cpu
            || execution_plan.plan.precision != NumericPrecision::Float32
        {
            return Err(RawNindFoundationProviderConfigurationError::ExpectedCpuFloat32Plan);
        }
        if execution_plan.plan.backend_id != installation.backend_id {
            return Err(RawNindFoundationProviderConfigurationError::RuntimePlanMismatch);
        }
        if foundation_plan.runtime_version != installation.runtime_version
            || foundation_plan.source_pixel_contract_sha256
                != RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256
        {
            return Err(RawNindFoundationProviderConfigurationError::FoundationPlanMismatch);
        }
        if input_raw.as_os_str().is_empty() || output_foundation.as_os_str().is_empty() {
            return Err(RawNindFoundationProviderConfigurationError::EmptyPath);
        }
        if input_raw == output_foundation {
            return Err(RawNindFoundationProviderConfigurationError::InputOutputConflict);
        }
        let route = ExecutionRouteIdentity {
            contract_version: crate::EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider: installation.provider_identity.clone(),
            model: installation.model_identity.clone(),
        };
        route.validate()?;
        Ok(Self {
            route,
            execution_plan,
            foundation_plan,
            executable: installation.executable.clone(),
            model_package: installation.model_package.clone(),
            model_graph: installation.model_graph.clone(),
            manifest_path: installation.manifest_path.clone(),
            runtime_version: installation.runtime_version.clone(),
            input_raw,
            output_foundation,
            output_verifier,
            execution_started: Mutex::new(false),
        })
    }

    pub const fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    pub const fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    pub fn proposal_path(&self) -> &Path {
        &self.output_foundation
    }

    fn begin_once(&self) -> bool {
        self.execution_started.lock().is_ok_and(|mut started| {
            if *started {
                false
            } else {
                *started = true;
                true
            }
        })
    }

    fn execute_foundation(
        &self,
        execution: &AdmittedExecution,
        cancellation: &CancellationToken,
        progress: &RuntimeProgressReporter<'_>,
    ) -> RawNindFoundationOutcome {
        let input = match self.preflight(execution, cancellation) {
            Ok(input) => input,
            Err(outcome) => return outcome,
        };
        publish_progress(progress, "preparing", 100);
        let receipt = match self.run_sidecar(cancellation, progress) {
            Ok(receipt) => receipt,
            Err(outcome) => return outcome,
        };
        self.verify_output(input, &receipt, progress)
    }

    fn preflight<'execution>(
        &self,
        execution: &'execution AdmittedExecution,
        cancellation: &CancellationToken,
    ) -> Result<&'execution crate::ArtifactReference, RawNindFoundationOutcome> {
        if !self.begin_once() {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "provider_reused",
            )));
        }
        if !self.executable.is_file() {
            return Err(RawNindFoundationOutcome::Unavailable(
                ProviderUnavailable::AdapterNotLinked,
            ));
        }
        if !self.model_package.is_file() || !self.model_graph.is_file() {
            return Err(RawNindFoundationOutcome::Unavailable(
                ProviderUnavailable::ModelNotInstalled,
            ));
        }
        if load_exact_manifest(&self.manifest_path).is_err() {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "model_manifest_changed",
            )));
        }
        if !self.input_raw.is_file() {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "input_missing",
            )));
        }
        if self.output_foundation.exists()
            || self
                .output_foundation
                .parent()
                .is_none_or(|parent| !parent.is_dir())
        {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "foundation_destination_invalid",
            )));
        }
        let Some(input) = exact_raw_foundation_input(execution) else {
            return Err(RawNindFoundationOutcome::Failed(
                RuntimeFailure::InvalidProviderOutput {
                    detail:
                        "RAW foundation requires one exact RawFile input and explicit parameters"
                            .into(),
                },
            ));
        };
        if self.foundation_plan.source_sha256 != input.content_hash
            || self.foundation_plan.source_size_bytes != input.byte_len
        {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "foundation_plan_source_mismatch",
            )));
        }
        let Ok(metadata) = fs::metadata(&self.input_raw) else {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "input_metadata_failed",
            )));
        };
        if metadata.len() != input.byte_len {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "input_identity_changed",
            )));
        }
        if cancellation.is_cancelled() {
            return Err(RawNindFoundationOutcome::Cancelled);
        }
        Ok(input)
    }

    fn run_sidecar(
        &self,
        cancellation: &CancellationToken,
        progress: &RuntimeProgressReporter<'_>,
    ) -> Result<FoundationReceipt, RawNindFoundationOutcome> {
        let Ok(child) = Command::new(&self.executable)
            .arg("--model-package")
            .arg(&self.model_package)
            .arg("--model-graph")
            .arg(&self.model_graph)
            .arg("--manifest")
            .arg(&self.manifest_path)
            .arg("--input-raw")
            .arg(&self.input_raw)
            .arg("--output-foundation")
            .arg(&self.output_foundation)
            .arg("--source-pixel-contract-sha256")
            .arg(RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256)
            .arg("--run")
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
        else {
            return Err(RawNindFoundationOutcome::Failed(provider_error(
                "sidecar_spawn_failed",
            )));
        };
        publish_progress(progress, "inference", 500);
        let captured = match wait_with_bounded_output(child, cancellation, POLL_INTERVAL) {
            Ok(captured) => captured,
            Err(ProcessFailure::Cancelled) => {
                return Err(self.discard_unpublished_output(RawNindFoundationOutcome::Cancelled));
            }
            Err(ProcessFailure::Wait | ProcessFailure::Reader) => {
                return Err(
                    self.discard_unpublished_output(RawNindFoundationOutcome::Failed(
                        provider_error("sidecar_wait_failed"),
                    )),
                );
            }
        };
        if !captured.status.success() {
            return Err(
                self.discard_unpublished_output(RawNindFoundationOutcome::Failed(provider_error(
                    "sidecar_execution_failed",
                ))),
            );
        }
        let Ok(receipt) = parse_foundation_receipt(&captured.stdout) else {
            return Err(
                self.discard_unpublished_output(RawNindFoundationOutcome::Failed(
                    RuntimeFailure::InvalidProviderOutput {
                        detail: "RawNIND sidecar returned a malformed foundation receipt".into(),
                    },
                )),
            );
        };
        Ok(receipt)
    }

    fn verify_output(
        &self,
        input: &crate::ArtifactReference,
        receipt: &FoundationReceipt,
        progress: &RuntimeProgressReporter<'_>,
    ) -> RawNindFoundationOutcome {
        publish_progress(progress, "verifying", 9_000);
        let Ok(verification) = self.output_verifier.verify(&self.output_foundation) else {
            return self.discard_unpublished_output(RawNindFoundationOutcome::Failed(
                RuntimeFailure::InvalidProviderOutput {
                    detail: "RawNIND sidecar output failed complete foundation verification".into(),
                },
            ));
        };
        let Ok(descriptor) = descriptor_from_verification(
            verification,
            input.content_hash.as_str(),
            input.byte_len,
            RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
            &self.runtime_version,
            &self.foundation_plan,
            receipt,
        ) else {
            return self.discard_unpublished_output(RawNindFoundationOutcome::Failed(
                RuntimeFailure::InvalidProviderOutput {
                    detail: "RawNIND foundation receipt or provenance did not match its bytes"
                        .into(),
                },
            ));
        };
        publish_progress(progress, "complete", crate::RUNTIME_PROGRESS_COMPLETE);
        RawNindFoundationOutcome::Succeeded(Box::new(descriptor))
    }

    fn discard_unpublished_output(
        &self,
        outcome: RawNindFoundationOutcome,
    ) -> RawNindFoundationOutcome {
        match fs::remove_file(&self.output_foundation) {
            Ok(()) => outcome,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => outcome,
            Err(_) => RawNindFoundationOutcome::Failed(provider_error(
                "foundation_partial_cleanup_failed",
            )),
        }
    }
}

impl RuntimeProvider for RawNindFoundationProvider {
    type Output = RawFoundationArtifact;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::RawFoundationDenoise
    }

    fn execute(
        &self,
        execution: &AdmittedExecution,
        cancellation: &CancellationToken,
        progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        let started = Instant::now();
        let outcome = self.execute_foundation(execution, cancellation, progress);
        let usage = elapsed_usage(started);
        match outcome {
            RawNindFoundationOutcome::Succeeded(output) => ProviderTerminal::Succeeded {
                output: *output,
                usage,
            },
            RawNindFoundationOutcome::Unavailable(reason) => {
                ProviderTerminal::Unavailable { reason, usage }
            }
            RawNindFoundationOutcome::Cancelled => ProviderTerminal::Cancelled { usage },
            RawNindFoundationOutcome::Failed(failure) => {
                ProviderTerminal::Failed { failure, usage }
            }
        }
    }
}

#[derive(Debug, Error)]
pub enum RawNindFoundationProviderConfigurationError {
    #[error("RawNIND foundation requires a CPU float32 execution plan")]
    ExpectedCpuFloat32Plan,
    #[error("RawNIND foundation execution plan belongs to another runtime")]
    RuntimePlanMismatch,
    #[error("RawNIND foundation cache plan belongs to another source or runtime")]
    FoundationPlanMismatch,
    #[error("RawNIND foundation provider paths must not be empty")]
    EmptyPath,
    #[error("RawNIND foundation input and output paths must differ")]
    InputOutputConflict,
    #[error(transparent)]
    InvalidRuntimeIdentity(#[from] RuntimeContractError),
}

#[derive(Debug, Error)]
pub enum RawNindFoundationModelVerificationError {
    #[error("RawNIND foundation provider executable is not installed")]
    AdapterNotLinked,
    #[error("RawNIND public model is not installed")]
    ModelNotInstalled,
    #[error("RawNIND public model verification was cancelled")]
    Cancelled,
    #[error("RawNIND public model manifest could not be read")]
    ManifestRead,
    #[error("RawNIND public model manifest is invalid: {0}")]
    ManifestInvalid(String),
    #[error("RawNIND native model verification failed")]
    VerificationFailed,
}

#[derive(Debug, Error)]
pub enum RawNindFoundationPlanningError {
    #[error("RawNIND foundation source identity is invalid")]
    InvalidSourceIdentity,
    #[error("RawNIND foundation source is unavailable")]
    SourceUnavailable,
    #[error("RawNIND foundation source changed while planning")]
    SourceIdentityChanged,
    #[error("RawNIND foundation planning was cancelled")]
    Cancelled,
    #[error("RawNIND foundation planning failed")]
    PlanningFailed,
}

#[derive(Debug, Error)]
enum RawNindFoundationOutputVerificationError {
    #[error("foundation artifact failed verification")]
    InvalidArtifact,
}

enum RawNindFoundationOutcome {
    Succeeded(Box<RawFoundationArtifact>),
    Unavailable(ProviderUnavailable),
    Cancelled,
    Failed(RuntimeFailure),
}

struct FoundationReceipt {
    cache_key_sha256: String,
    artifact_identity_sha256: String,
    file_sha256: String,
    width: u32,
    height: u32,
    runtime_version: String,
}

fn exact_raw_foundation_input(execution: &AdmittedExecution) -> Option<&crate::ArtifactReference> {
    if execution.request().task != AiTaskKind::MaterializeRawFoundation
        || execution.request().parameters != AiTaskParameters::RawFoundation
        || execution.request().inputs.len() != 1
    {
        return None;
    }
    let input = &execution.request().inputs[0];
    (input.role == InputRole::RawFile).then_some(input)
}

fn descriptor_from_verification(
    verification: FoundationArtifactVerification,
    expected_source_sha256: &str,
    expected_source_size_bytes: u64,
    source_pixel_contract_sha256: &str,
    expected_runtime_version: &str,
    plan: &RawNindFoundationPlan,
    receipt: &FoundationReceipt,
) -> Result<RawFoundationArtifact, ()> {
    if verification.source_sha256 != expected_source_sha256
        || verification.source_size_bytes != expected_source_size_bytes
        || verification.source_pixel_contract_sha256 != source_pixel_contract_sha256
        || receipt.cache_key_sha256 != verification.cache_key_sha256
        || receipt.artifact_identity_sha256 != verification.artifact_identity_sha256
        || receipt.file_sha256 != verification.file_sha256
        || receipt.width != verification.width
        || receipt.height != verification.height
        || receipt.runtime_version != expected_runtime_version
        || plan.runtime_version != expected_runtime_version
    {
        return Err(());
    }
    descriptor_from_planned_verification(verification, plan)
}

pub(crate) fn descriptor_from_planned_verification(
    verification: FoundationArtifactVerification,
    plan: &RawNindFoundationPlan,
) -> Result<RawFoundationArtifact, ()> {
    if verification.model_package_sha256 != RAWNIND_FOUNDATION_PACKAGE_SHA256
        || verification.model_graph_sha256 != RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256
        || verification.implementation_revision != RAWNIND_FOUNDATION_IMPLEMENTATION_REVISION
        || plan.cache_key_sha256 != verification.cache_key_sha256
        || plan.source_sha256 != verification.source_sha256
        || plan.source_size_bytes != verification.source_size_bytes
        || plan.source_pixel_contract_sha256 != verification.source_pixel_contract_sha256
        || plan.raster_extent.width != verification.width
        || plan.raster_extent.height != verification.height
    {
        return Err(());
    }
    let artifact = GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Sha256,
        verification.file_sha256,
        verification.file_bytes,
        RAW_FOUNDATION_MEDIA_TYPE.into(),
        RAW_FOUNDATION_ENCODING_VERSION,
    )
    .map_err(|_| ())?;
    let extent = RasterExtent::new(verification.width, verification.height).map_err(|_| ())?;
    let source = RawFoundationSourceProvenance::new(
        verification.source_sha256,
        verification.source_size_bytes,
        verification.source_pixel_contract_sha256,
    )
    .map_err(|_| ())?;
    let provenance = RawFoundationProvenance::new(
        verification.cache_key_sha256,
        verification.artifact_identity_sha256,
        verification.model_package_sha256,
        verification.model_graph_sha256,
        verification.implementation_revision,
    )
    .map_err(|_| ())?;
    RawFoundationArtifact::new(artifact, extent, source, provenance).map_err(|_| ())
}

fn parse_model_receipt(bytes: &[u8]) -> Result<String, ()> {
    let text = std::str::from_utf8(bytes).map_err(|_| ())?.trim();
    let fields: Vec<_> = text.split_ascii_whitespace().collect();
    if fields.len() != 4 || fields[0] != RAWNIND_FOUNDATION_MODEL_RECEIPT_PREFIX {
        return Err(());
    }
    let package = value_field(fields[1], "package_sha256")?;
    let graph = value_field(fields[2], "graph_sha256")?;
    let runtime = value_field(fields[3], "runtime_version")?;
    if package != RAWNIND_FOUNDATION_PACKAGE_SHA256
        || graph != RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256
        || !valid_runtime_version(runtime)
    {
        return Err(());
    }
    Ok(runtime.into())
}

fn parse_plan_receipt(bytes: &[u8]) -> Result<RawNindFoundationPlan, ()> {
    let text = std::str::from_utf8(bytes).map_err(|_| ())?.trim();
    let fields: Vec<_> = text.split_ascii_whitespace().collect();
    if fields.len() != 8 || fields[0] != RAWNIND_FOUNDATION_PLAN_RECEIPT_PREFIX {
        return Err(());
    }
    let cache_key_sha256 = value_field(fields[1], "cache_key_sha256")?;
    let source_sha256 = value_field(fields[2], "source_sha256")?;
    let source_size_bytes = value_field(fields[3], "source_size_bytes")?
        .parse()
        .map_err(|_| ())?;
    let source_pixel_contract_sha256 = value_field(fields[4], "source_pixel_contract_sha256")?;
    let width = value_field(fields[5], "width")?.parse().map_err(|_| ())?;
    let height = value_field(fields[6], "height")?.parse().map_err(|_| ())?;
    let runtime_version = value_field(fields[7], "runtime_version")?;
    validate_lower_sha256(cache_key_sha256)?;
    validate_lower_sha256(source_sha256)?;
    validate_lower_sha256(source_pixel_contract_sha256)?;
    if source_size_bytes == 0 || !valid_runtime_version(runtime_version) {
        return Err(());
    }
    Ok(RawNindFoundationPlan {
        cache_key_sha256: cache_key_sha256.into(),
        source_sha256: source_sha256.into(),
        source_size_bytes,
        source_pixel_contract_sha256: source_pixel_contract_sha256.into(),
        raster_extent: RasterExtent::new(width, height).map_err(|_| ())?,
        runtime_version: runtime_version.into(),
    })
}

fn parse_foundation_receipt(bytes: &[u8]) -> Result<FoundationReceipt, ()> {
    let text = std::str::from_utf8(bytes).map_err(|_| ())?.trim();
    let fields: Vec<_> = text.split_ascii_whitespace().collect();
    if fields.len() != 7 || fields[0] != RAWNIND_FOUNDATION_RECEIPT_PREFIX {
        return Err(());
    }
    let cache_key_sha256 = value_field(fields[1], "cache_key_sha256")?;
    let artifact_identity_sha256 = value_field(fields[2], "artifact_identity_sha256")?;
    let file_sha256 = value_field(fields[3], "file_sha256")?;
    validate_lower_sha256(cache_key_sha256)?;
    validate_lower_sha256(artifact_identity_sha256)?;
    validate_lower_sha256(file_sha256)?;
    let width = value_field(fields[4], "width")?.parse().map_err(|_| ())?;
    let height = value_field(fields[5], "height")?.parse().map_err(|_| ())?;
    let runtime_version = value_field(fields[6], "runtime_version")?;
    if width == 0 || height == 0 || !valid_runtime_version(runtime_version) {
        return Err(());
    }
    Ok(FoundationReceipt {
        cache_key_sha256: cache_key_sha256.into(),
        artifact_identity_sha256: artifact_identity_sha256.into(),
        file_sha256: file_sha256.into(),
        width,
        height,
        runtime_version: runtime_version.into(),
    })
}

fn load_exact_manifest(
    path: &Path,
) -> Result<ModelManifest, RawNindFoundationModelVerificationError> {
    let bytes =
        fs::read(path).map_err(|_| RawNindFoundationModelVerificationError::ManifestRead)?;
    let manifest: ModelManifest = serde_json::from_slice(&bytes).map_err(|error| {
        RawNindFoundationModelVerificationError::ManifestInvalid(error.to_string())
    })?;
    let expected: ModelManifest = serde_json::from_slice(CHECKED_IN_MANIFEST).map_err(|error| {
        RawNindFoundationModelVerificationError::ManifestInvalid(error.to_string())
    })?;
    if manifest != expected
        || manifest.model_id != RAWNIND_FOUNDATION_MODEL_ID
        || manifest.exact_revision != RAWNIND_FOUNDATION_MODEL_REVISION
        || manifest.artifact_set.inventory_blake3 != RAWNIND_FOUNDATION_ARTIFACT_SET_BLAKE3
        || manifest.format != ModelFormat::Onnx
        || manifest.opset != Some(20)
        || !manifest
            .capabilities
            .contains(&AiCapability::RawFoundationDenoise)
        || manifest.artifact_set.artifacts.len() != 2
    {
        return Err(RawNindFoundationModelVerificationError::ManifestInvalid(
            "identity, tensors, execution target, or capability changed".into(),
        ));
    }
    Ok(manifest)
}

fn model_identity(manifest: &ModelManifest) -> AdmittedModelIdentity {
    AdmittedModelIdentity::LocalArtifactSet {
        model_id: manifest.model_id.clone(),
        exact_revision: manifest.exact_revision.clone(),
        artifact_set_blake3: manifest.artifact_set.inventory_blake3.clone(),
        preprocessing_version: manifest.preprocessing_version.clone(),
    }
}

fn value_field<'a>(field: &'a str, name: &str) -> Result<&'a str, ()> {
    field
        .strip_prefix(name)
        .and_then(|value| value.strip_prefix('='))
        .ok_or(())
}

fn validate_lower_sha256(value: &str) -> Result<(), ()> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(());
    }
    Ok(())
}

fn valid_runtime_version(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b'+'))
}

fn publish_progress(progress: &RuntimeProgressReporter<'_>, phase: &str, basis_points: u16) {
    let _ = progress.publish(RuntimeProgress {
        phase_code: phase.into(),
        completed_basis_points: basis_points,
    });
}

fn provider_error(code: &str) -> RuntimeFailure {
    RuntimeFailure::ProviderError { code: code.into() }
}

fn elapsed_usage(started: Instant) -> RuntimeUsage {
    RuntimeUsage {
        elapsed_ms: u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
        ..RuntimeUsage::default()
    }
}

#[cfg(test)]
mod tests;

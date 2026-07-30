//! Local-process adapter for side-loaded SAM 2.1 Core ML packages.
//!
//! The provider executable and model packages remain separate local payloads.
//! This owner binds their exact admitted identity to a bounded process
//! invocation and returns only a verified soft-mask proposal. Product callers
//! may share one resident process and image embedding across request-scoped
//! provider values; the one-shot constructor remains available for isolated
//! compatibility checks.

mod resident;

use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::Mutex,
    time::Instant,
};

use thiserror::Error;

use crate::{
    AdmittedExecution, AdmittedModelIdentity, AiCapability, AiGeneratedPayload, AiTaskParameters,
    ArtifactHashAlgorithm, BackendKind, CancellationToken,
    EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION, ExecutionPlanIdentity, ExecutionRouteIdentity,
    GeneratedArtifactReference, MaskPointPolarity, MaskPrompt, MaskPromptPoint, MaskSemantic,
    ModelFormat, ModelManifest, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    ProviderUnavailable, RasterExtent, RuntimeContractError, RuntimeFailure, RuntimeProgress,
    RuntimeProgressReporter, RuntimeProvider, RuntimeUsage, SoftMaskArtifact, SoftMaskEncoding,
    SubjectMaskParameters,
};

use self::resident::ResidentSessionFailure;
use super::local_process::{CapturedChild, ProcessFailure, wait_with_bounded_output};

pub use self::resident::Sam2CoreMlResidentSession;

pub const SAM2_COREML_PROVIDER_ID: &str = "shadow.sam2.coreml-sidecar";
pub const SAM2_COREML_ADAPTER_REVISION: &str = "sam2-coreml-mask-protocol-v1";
pub const SAM2_COREML_RECEIPT_PREFIX: &str = "shadow-sam2-coreml-mask-v1";
pub const SAM2_COREML_MODEL_RECEIPT_PREFIX: &str = "shadow-sam2-coreml-model-v1";
pub const SAM2_COREML_MODEL_ID: &str = "apple/coreml-sam2.1-small";
pub const SAM2_COREML_EXACT_REVISION: &str = "883f5787eb0be35ce6965907a8bc1f5320a5a02e";
pub const SAM2_COREML_ARTIFACT_SET_BLAKE3: &str =
    "28982b31b020249ca62f9c41fa82193f794b215d07b7080a3c00e5c85733bca0";
pub const SAM2_COREML_MAX_PROMPT_POINTS: usize = 16;

const MASK_EDGE: u32 = 256;
const MASK_BYTE_LEN: u64 = 65_536;
const SOFT_MASK_MEDIA_TYPE: &str = "application/x-shadow-soft-mask";
const SOFT_MASK_ENCODING_VERSION: u32 = 1;
const POLL_INTERVAL_MS: u64 = 10;
const MODEL_PACKAGES: [&str; 3] = [
    "SAM2_1SmallImageEncoderFLOAT16.mlpackage",
    "SAM2_1SmallPromptEncoderFLOAT16.mlpackage",
    "SAM2_1SmallMaskDecoderFLOAT16.mlpackage",
];

/// One request-scoped binding to the local SAM sidecar.
///
/// The output path must be unique and absent. A provider instance is
/// deliberately single-use even if a caller attempts to reuse it.
#[derive(Debug)]
pub struct Sam2CoreMlSidecarProvider {
    route: ExecutionRouteIdentity,
    execution_plan: ExecutionPlanIdentity,
    executable: PathBuf,
    model_directory: PathBuf,
    manifest_path: PathBuf,
    input_jpeg: PathBuf,
    output_mask: PathBuf,
    execution_mode: SidecarExecutionMode,
    execution_started: Mutex<bool>,
}

#[derive(Debug)]
enum SidecarExecutionMode {
    OneShot,
    Resident {
        session: Sam2CoreMlResidentSession,
        input_content_hash: String,
    },
}

/// Model-installation authority issued only after the native sidecar verifies
/// every package member against Shadow's exact manifest.
#[derive(Debug)]
pub struct VerifiedSam2CoreMlInstallation {
    manifest: ModelManifest,
    model_identity: AdmittedModelIdentity,
    executable: PathBuf,
    model_directory: PathBuf,
    manifest_path: PathBuf,
}

impl VerifiedSam2CoreMlInstallation {
    pub const fn manifest(&self) -> &ModelManifest {
        &self.manifest
    }

    pub const fn model_identity(&self) -> &AdmittedModelIdentity {
        &self.model_identity
    }

    pub fn executable(&self) -> &Path {
        &self.executable
    }

    pub fn model_directory(&self) -> &Path {
        &self.model_directory
    }

    pub fn manifest_path(&self) -> &Path {
        &self.manifest_path
    }
}

/// Verifies one side-loaded SAM installation before resource admission.
///
/// Rust validates the manifest identity and schema; the native sidecar then
/// checks the exact length and SHA-256 of all executable package members.
///
/// # Errors
///
/// Returns an error for an unavailable adapter/model, a changed manifest,
/// cancellation, or any native verification failure.
pub fn verify_sam2_coreml_installation(
    executable: impl Into<PathBuf>,
    model_directory: impl Into<PathBuf>,
    manifest_path: impl Into<PathBuf>,
    cancellation: &CancellationToken,
) -> Result<VerifiedSam2CoreMlInstallation, Sam2CoreMlModelVerificationError> {
    if !cfg!(target_os = "macos") {
        return Err(Sam2CoreMlModelVerificationError::PlatformUnsupported);
    }
    let executable = executable.into();
    let model_directory = model_directory.into();
    let manifest_path = manifest_path.into();
    if !executable.is_file() {
        return Err(Sam2CoreMlModelVerificationError::AdapterNotLinked);
    }
    if !model_directory.is_dir() {
        return Err(Sam2CoreMlModelVerificationError::ModelNotInstalled);
    }
    let manifest = load_exact_manifest(&manifest_path)?;
    if cancellation.is_cancelled() {
        return Err(Sam2CoreMlModelVerificationError::Cancelled);
    }

    let mut command = Command::new(&executable);
    command
        .arg("--model-dir")
        .arg(&model_directory)
        .arg("--manifest")
        .arg(&manifest_path)
        .arg("--verify-model")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let child = command
        .spawn()
        .map_err(|_| Sam2CoreMlModelVerificationError::VerificationFailed)?;
    let captured = wait_with_bounded_output(
        child,
        cancellation,
        std::time::Duration::from_millis(POLL_INTERVAL_MS),
    )
    .map_err(|failure| match failure {
        ProcessFailure::Cancelled => Sam2CoreMlModelVerificationError::Cancelled,
        ProcessFailure::Wait | ProcessFailure::Reader => {
            Sam2CoreMlModelVerificationError::VerificationFailed
        }
    })?;
    if !captured.status.success() || parse_model_receipt(&captured.stdout).is_err() {
        return Err(Sam2CoreMlModelVerificationError::VerificationFailed);
    }
    Ok(VerifiedSam2CoreMlInstallation {
        model_identity: model_identity(&manifest),
        manifest,
        executable,
        model_directory,
        manifest_path,
    })
}

impl Sam2CoreMlSidecarProvider {
    /// Constructs a request-scoped provider around one exact admitted model.
    ///
    /// # Errors
    ///
    /// Returns an error for a non-Core-ML plan, empty or conflicting paths, or
    /// an invalid route/plan identity.
    pub fn new(
        installation: &VerifiedSam2CoreMlInstallation,
        execution_plan: ExecutionPlanIdentity,
        input_jpeg: impl Into<PathBuf>,
        output_mask: impl Into<PathBuf>,
    ) -> Result<Self, Sam2CoreMlProviderConfigurationError> {
        Self::new_with_mode(
            installation,
            execution_plan,
            input_jpeg.into(),
            output_mask.into(),
            SidecarExecutionMode::OneShot,
        )
    }

    /// Constructs a request-scoped provider that executes through one shared
    /// resident model process and content-addressed image embedding.
    ///
    /// # Errors
    ///
    /// Returns an error when the session belongs to another admitted model,
    /// the input identity is empty, or the ordinary provider contract is
    /// invalid.
    pub fn new_resident(
        installation: &VerifiedSam2CoreMlInstallation,
        execution_plan: ExecutionPlanIdentity,
        input_jpeg: impl Into<PathBuf>,
        input_content_hash: impl Into<String>,
        output_mask: impl Into<PathBuf>,
        session: Sam2CoreMlResidentSession,
    ) -> Result<Self, Sam2CoreMlProviderConfigurationError> {
        let input_content_hash = input_content_hash.into();
        if input_content_hash.is_empty() {
            return Err(Sam2CoreMlProviderConfigurationError::EmptyInputIdentity);
        }
        if !session.matches_installation(installation) {
            return Err(Sam2CoreMlProviderConfigurationError::ResidentInstallationMismatch);
        }
        Self::new_with_mode(
            installation,
            execution_plan,
            input_jpeg.into(),
            output_mask.into(),
            SidecarExecutionMode::Resident {
                session,
                input_content_hash,
            },
        )
    }

    fn new_with_mode(
        installation: &VerifiedSam2CoreMlInstallation,
        execution_plan: ExecutionPlanIdentity,
        input_jpeg: PathBuf,
        output_mask: PathBuf,
        execution_mode: SidecarExecutionMode,
    ) -> Result<Self, Sam2CoreMlProviderConfigurationError> {
        execution_plan.validate()?;
        if execution_plan.plan.backend_kind != BackendKind::CoreMl {
            return Err(Sam2CoreMlProviderConfigurationError::ExpectedCoreMlPlan);
        }

        if [input_jpeg.as_path(), output_mask.as_path()]
            .iter()
            .any(|path| path.as_os_str().is_empty())
        {
            return Err(Sam2CoreMlProviderConfigurationError::EmptyPath);
        }
        if input_jpeg == output_mask {
            return Err(Sam2CoreMlProviderConfigurationError::InputOutputConflict);
        }

        let route = ExecutionRouteIdentity {
            contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider: ProviderIdentity {
                provider_id: SAM2_COREML_PROVIDER_ID.into(),
                adapter_revision: SAM2_COREML_ADAPTER_REVISION.into(),
                execution_class: ProviderExecutionClass::LocalModel,
            },
            model: installation.model_identity.clone(),
        };
        route.validate()?;
        Ok(Self {
            route,
            execution_plan,
            executable: installation.executable.clone(),
            model_directory: installation.model_directory.clone(),
            manifest_path: installation.manifest_path.clone(),
            input_jpeg,
            output_mask,
            execution_mode,
            execution_started: Mutex::new(false),
        })
    }

    pub const fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    pub const fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    /// Rebuildable provider output to stage after a successful runtime receipt.
    pub fn proposal_path(&self) -> &Path {
        &self.output_mask
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

    fn execute_subject_mask(
        &self,
        parameters: &SubjectMaskParameters,
        cancellation: &CancellationToken,
    ) -> SidecarOutcome {
        if !cfg!(target_os = "macos") {
            return SidecarOutcome::Unavailable(ProviderUnavailable::PlatformUnsupported);
        }
        if !self.executable.is_file() {
            return SidecarOutcome::Unavailable(ProviderUnavailable::AdapterNotLinked);
        }
        if !self.model_directory.is_dir()
            || MODEL_PACKAGES
                .iter()
                .any(|package| !self.model_directory.join(package).is_dir())
        {
            return SidecarOutcome::Unavailable(ProviderUnavailable::ModelNotInstalled);
        }
        let manifest_matches_route = load_exact_manifest(&self.manifest_path)
            .is_ok_and(|manifest| model_identity(&manifest) == self.route.model);
        if !manifest_matches_route {
            return SidecarOutcome::Failed(provider_error("model_manifest_changed"));
        }
        if !self.input_jpeg.is_file() {
            return SidecarOutcome::Failed(provider_error("input_missing"));
        }
        if self.output_mask.exists() {
            return SidecarOutcome::Failed(provider_error("proposal_output_exists"));
        }
        if self
            .output_mask
            .parent()
            .is_none_or(|parent| !parent.is_dir())
        {
            return SidecarOutcome::Failed(provider_error("proposal_parent_missing"));
        }

        let MaskPrompt::Points { points } = &parameters.prompt else {
            return SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                detail: "SAM 2.1 Core ML requires point prompts".into(),
            });
        };
        if points.len() > SAM2_COREML_MAX_PROMPT_POINTS {
            return SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                detail: format!(
                    "SAM 2.1 Core ML accepts at most {SAM2_COREML_MAX_PROMPT_POINTS} points"
                ),
            });
        }
        if cancellation.is_cancelled() {
            return SidecarOutcome::Cancelled;
        }

        match &self.execution_mode {
            SidecarExecutionMode::OneShot => {
                self.execute_one_shot(parameters, points, cancellation)
            }
            SidecarExecutionMode::Resident {
                session,
                input_content_hash,
            } => self.execute_resident(
                parameters,
                points,
                session,
                input_content_hash,
                cancellation,
            ),
        }
    }

    fn execute_one_shot(
        &self,
        parameters: &SubjectMaskParameters,
        points: &[MaskPromptPoint],
        cancellation: &CancellationToken,
    ) -> SidecarOutcome {
        let mut command = Command::new(&self.executable);
        command
            .arg("--model-dir")
            .arg(&self.model_directory)
            .arg("--manifest")
            .arg(&self.manifest_path)
            .arg("--input-jpeg")
            .arg(&self.input_jpeg)
            .arg("--output-mask")
            .arg(&self.output_mask)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        for point in points {
            command
                .arg("--point")
                .arg(format!("{:.17}", point.x.get()))
                .arg(format!("{:.17}", point.y.get()))
                .arg(match point.polarity {
                    MaskPointPolarity::Foreground => "foreground",
                    MaskPointPolarity::Background => "background",
                });
        }

        let Ok(child) = command.spawn() else {
            return SidecarOutcome::Failed(provider_error("sidecar_spawn_failed"));
        };
        let captured = match wait_with_bounded_output(
            child,
            cancellation,
            std::time::Duration::from_millis(POLL_INTERVAL_MS),
        ) {
            Ok(captured) => captured,
            Err(ProcessFailure::Cancelled) => {
                remove_failed_output(&self.output_mask);
                return SidecarOutcome::Cancelled;
            }
            Err(ProcessFailure::Wait | ProcessFailure::Reader) => {
                remove_failed_output(&self.output_mask);
                return SidecarOutcome::Failed(provider_error("sidecar_wait_failed"));
            }
        };
        self.finish_output(parameters, points.len(), &captured)
    }

    fn execute_resident(
        &self,
        parameters: &SubjectMaskParameters,
        points: &[MaskPromptPoint],
        session: &Sam2CoreMlResidentSession,
        input_content_hash: &str,
        cancellation: &CancellationToken,
    ) -> SidecarOutcome {
        match session.predict(
            &self.input_jpeg,
            input_content_hash,
            &self.output_mask,
            points,
            cancellation,
        ) {
            Ok(receipt)
                if receipt.width == MASK_EDGE
                    && receipt.height == MASK_EDGE
                    && receipt.points == points.len()
                    && receipt.score.is_finite()
                    && (0.0..=1.0).contains(&receipt.score) =>
            {
                self.finish_verified_output(parameters)
            }
            Ok(_) => {
                remove_failed_output(&self.output_mask);
                SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                    detail: "SAM 2.1 resident sidecar returned a malformed receipt".into(),
                })
            }
            Err(ResidentSessionFailure::Cancelled) => {
                remove_failed_output(&self.output_mask);
                SidecarOutcome::Cancelled
            }
            Err(ResidentSessionFailure::StatePoisoned) => {
                SidecarOutcome::Failed(provider_error("resident_state_poisoned"))
            }
            Err(
                ResidentSessionFailure::InvalidInputIdentity | ResidentSessionFailure::InvalidPath,
            ) => SidecarOutcome::Failed(provider_error("resident_input_invalid")),
            Err(ResidentSessionFailure::ProviderRejected) => {
                SidecarOutcome::Failed(provider_error("resident_provider_rejected"))
            }
            Err(
                failure @ (ResidentSessionFailure::Spawn
                | ResidentSessionFailure::Transport
                | ResidentSessionFailure::Timeout
                | ResidentSessionFailure::InvalidResponse(_)
                | ResidentSessionFailure::RequestIdentityExhausted),
            ) => SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                detail: format!("SAM 2.1 resident session failed: {failure}"),
            }),
        }
    }

    fn finish_output(
        &self,
        parameters: &SubjectMaskParameters,
        point_count: usize,
        captured: &CapturedChild,
    ) -> SidecarOutcome {
        if !captured.status.success() {
            remove_failed_output(&self.output_mask);
            return SidecarOutcome::Failed(provider_error("sidecar_execution_failed"));
        }
        if parse_receipt(&captured.stdout, point_count).is_err() {
            remove_failed_output(&self.output_mask);
            return SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                detail: "SAM 2.1 Core ML sidecar returned a malformed receipt".into(),
            });
        }
        self.finish_verified_output(parameters)
    }

    fn finish_verified_output(&self, parameters: &SubjectMaskParameters) -> SidecarOutcome {
        let metadata = match fs::metadata(&self.output_mask) {
            Ok(metadata) if metadata.is_file() && metadata.len() == MASK_BYTE_LEN => metadata,
            _ => {
                remove_failed_output(&self.output_mask);
                return SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                    detail: "SAM 2.1 Core ML sidecar returned an invalid mask byte length".into(),
                });
            }
        };
        let bytes = match fs::read(&self.output_mask) {
            Ok(bytes) if bytes.len() as u64 == metadata.len() => bytes,
            _ => {
                remove_failed_output(&self.output_mask);
                return SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                    detail: "SAM 2.1 Core ML mask changed while being verified".into(),
                });
            }
        };
        let Ok(artifact) = GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Blake3_256,
            blake3::hash(&bytes).to_hex().to_string(),
            MASK_BYTE_LEN,
            SOFT_MASK_MEDIA_TYPE.into(),
            SOFT_MASK_ENCODING_VERSION,
        ) else {
            remove_failed_output(&self.output_mask);
            return SidecarOutcome::Failed(RuntimeFailure::InvalidProviderOutput {
                detail: "SAM 2.1 Core ML mask identity is invalid".into(),
            });
        };
        SidecarOutcome::Succeeded(AiGeneratedPayload::SoftMask(SoftMaskArtifact {
            artifact,
            raster_extent: RasterExtent {
                width: MASK_EDGE,
                height: MASK_EDGE,
            },
            coordinate_extent: parameters.coordinate_extent,
            coordinate_space: parameters.coordinate_space,
            encoding: SoftMaskEncoding::Gray8Unorm,
            semantic: MaskSemantic::UserPrompt,
        }))
    }
}

impl RuntimeProvider for Sam2CoreMlSidecarProvider {
    type Output = AiGeneratedPayload;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::SubjectMask
    }

    fn execute(
        &self,
        execution: &AdmittedExecution,
        cancellation: &CancellationToken,
        progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        let started = Instant::now();
        if !self.begin_once() {
            return ProviderTerminal::Failed {
                failure: provider_error("provider_reused"),
                usage: elapsed_usage(started),
            };
        }
        if progress
            .publish(RuntimeProgress {
                phase_code: "starting_provider".into(),
                completed_basis_points: 1_000,
            })
            .is_err()
        {
            return ProviderTerminal::Failed {
                failure: provider_error("progress_rejected"),
                usage: elapsed_usage(started),
            };
        }

        let AiTaskParameters::SubjectMask(parameters) = &execution.request().parameters else {
            return ProviderTerminal::Failed {
                failure: RuntimeFailure::InvalidProviderOutput {
                    detail: "SAM 2.1 Core ML received non-mask parameters".into(),
                },
                usage: elapsed_usage(started),
            };
        };
        let outcome = self.execute_subject_mask(parameters, cancellation);
        let usage = elapsed_usage(started);
        match outcome {
            SidecarOutcome::Succeeded(output) => {
                if progress
                    .publish(RuntimeProgress {
                        phase_code: "provider_output_verified".into(),
                        completed_basis_points: 10_000,
                    })
                    .is_err()
                {
                    remove_failed_output(&self.output_mask);
                    return ProviderTerminal::Failed {
                        failure: provider_error("progress_rejected"),
                        usage,
                    };
                }
                ProviderTerminal::Succeeded { output, usage }
            }
            SidecarOutcome::Unavailable(reason) => ProviderTerminal::Unavailable { reason, usage },
            SidecarOutcome::Cancelled => ProviderTerminal::Cancelled { usage },
            SidecarOutcome::Failed(failure) => ProviderTerminal::Failed { failure, usage },
        }
    }
}

#[derive(Debug, Error)]
pub enum Sam2CoreMlProviderConfigurationError {
    #[error("SAM 2.1 Core ML requires an admitted Core ML execution plan")]
    ExpectedCoreMlPlan,
    #[error("SAM 2.1 Core ML provider paths must not be empty")]
    EmptyPath,
    #[error("SAM 2.1 Core ML input and output paths must differ")]
    InputOutputConflict,
    #[error("SAM 2.1 Core ML resident input identity must not be empty")]
    EmptyInputIdentity,
    #[error("SAM 2.1 Core ML resident session belongs to another admitted model")]
    ResidentInstallationMismatch,
    #[error(transparent)]
    InvalidRuntimeIdentity(#[from] RuntimeContractError),
}

#[derive(Debug, Error)]
pub enum Sam2CoreMlModelVerificationError {
    #[error("SAM 2.1 Core ML is supported only on macOS")]
    PlatformUnsupported,
    #[error("SAM 2.1 Core ML provider executable is not installed")]
    AdapterNotLinked,
    #[error("SAM 2.1 Core ML model directory is not installed")]
    ModelNotInstalled,
    #[error("SAM 2.1 Core ML verification was cancelled")]
    Cancelled,
    #[error("SAM 2.1 Core ML manifest could not be read")]
    ManifestRead,
    #[error("SAM 2.1 Core ML manifest is invalid: {0}")]
    ManifestInvalid(String),
    #[error("SAM 2.1 Core ML native verification failed")]
    VerificationFailed,
}

enum SidecarOutcome {
    Succeeded(AiGeneratedPayload),
    Unavailable(ProviderUnavailable),
    Cancelled,
    Failed(RuntimeFailure),
}

fn parse_receipt(bytes: &[u8], expected_points: usize) -> Result<(), ()> {
    let text = std::str::from_utf8(bytes).map_err(|_| ())?.trim();
    let fields: Vec<_> = text.split_ascii_whitespace().collect();
    if fields.len() != 5 || fields[0] != SAM2_COREML_RECEIPT_PREFIX {
        return Err(());
    }
    let width = parse_field::<u32>(fields[1], "width")?;
    let height = parse_field::<u32>(fields[2], "height")?;
    let score = parse_field::<f64>(fields[3], "score")?;
    let points = parse_field::<usize>(fields[4], "points")?;
    if width != MASK_EDGE
        || height != MASK_EDGE
        || !score.is_finite()
        || !(0.0..=1.0).contains(&score)
        || points != expected_points
    {
        return Err(());
    }
    Ok(())
}

fn parse_model_receipt(bytes: &[u8]) -> Result<(), ()> {
    let text = std::str::from_utf8(bytes).map_err(|_| ())?.trim();
    let fields: Vec<_> = text.split_ascii_whitespace().collect();
    if fields.len() != 3 || fields[0] != SAM2_COREML_MODEL_RECEIPT_PREFIX {
        return Err(());
    }
    let revision = fields[1].strip_prefix("revision=").ok_or(())?;
    let files = parse_field::<usize>(fields[2], "files")?;
    if revision != SAM2_COREML_EXACT_REVISION || files != MODEL_PACKAGES.len() * 3 {
        return Err(());
    }
    Ok(())
}

fn load_exact_manifest(path: &Path) -> Result<ModelManifest, Sam2CoreMlModelVerificationError> {
    let bytes = fs::read(path).map_err(|_| Sam2CoreMlModelVerificationError::ManifestRead)?;
    let manifest: ModelManifest = serde_json::from_slice(&bytes)
        .map_err(|error| Sam2CoreMlModelVerificationError::ManifestInvalid(error.to_string()))?;
    if manifest.model_id != SAM2_COREML_MODEL_ID
        || manifest.exact_revision != SAM2_COREML_EXACT_REVISION
        || manifest.artifact_set.inventory_blake3 != SAM2_COREML_ARTIFACT_SET_BLAKE3
        || manifest.format != ModelFormat::CoreMlPackage
        || !manifest.capabilities.contains(&AiCapability::SubjectMask)
        || manifest.artifact_set.artifacts.len() != MODEL_PACKAGES.len() * 3
    {
        return Err(Sam2CoreMlModelVerificationError::ManifestInvalid(
            "identity or capability does not match the pinned provider".into(),
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

fn parse_field<T: std::str::FromStr>(field: &str, name: &str) -> Result<T, ()> {
    field
        .strip_prefix(name)
        .and_then(|value| value.strip_prefix('='))
        .ok_or(())?
        .parse()
        .map_err(|_| ())
}

fn provider_error(code: &str) -> RuntimeFailure {
    RuntimeFailure::ProviderError { code: code.into() }
}

fn remove_failed_output(path: &Path) {
    let _ = fs::remove_file(path);
}

fn elapsed_usage(started: Instant) -> RuntimeUsage {
    RuntimeUsage {
        elapsed_ms: u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
        ..RuntimeUsage::default()
    }
}

#[cfg(test)]
mod tests;

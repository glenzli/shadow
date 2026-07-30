//! Desktop application adapter for one admitted SAM subject-mask execution.
//!
//! Prepared JPEG rendering remains a desktop-session responsibility. This
//! owner verifies the installed model, constructs the provider-neutral job,
//! performs local resource admission, executes the sidecar, and stages the
//! resulting bytes without applying them to a Recipe.

pub(crate) mod config;
pub(crate) mod geometry;

use std::{
    fs::{self, OpenOptions},
    io::Write as _,
    path::Path,
    path::PathBuf,
    process::Command,
    str::FromStr,
    sync::{
        Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactReference,
    BackendKind, CancellationToken, ExecutionBackend, ExecutionLease, FallbackDisclosure,
    HardwareProfile, InputRole, LocalExecutionAdmission, LocalExecutionBinding,
    LocalModelAvailability, MaskPrompt, MaskPromptPoint, ModelAvailability, NumericPrecision,
    ObservationTarget, OnBatteryPolicy, PrivacyClass, ProviderExecutionClass, ProviderIdentity,
    RasterExtent, ResourceEstimate, ResourcePolicy, SAM2_COREML_ADAPTER_REVISION,
    SAM2_COREML_PROVIDER_ID, Sam2CoreMlProviderConfigurationError, Sam2CoreMlResidentSession,
    Sam2CoreMlSidecarProvider, SubjectMaskParameters, TaskPriority, UnitInterval,
    VerifiedSam2CoreMlInstallation, admit_local_execution, verify_sam2_coreml_installation,
};
use shadow_core::{
    DerivedRasterStageError, DerivedRasterStageReceipt, FilesystemDerivedRasterStore,
    execute_and_stage_derived_raster,
};
use shadow_domain::{MaskCoordinateSpace, PhotoId, Platform};
use thiserror::Error;

const MAX_PROVIDER_INPUT_JPEG_BYTES: usize = 64 * 1024 * 1024;
const MEBIBYTE: u64 = 1024 * 1024;
const GIBIBYTE: u64 = 1024 * MEBIBYTE;

#[derive(Debug)]
pub(crate) struct SubjectMaskRuntime {
    executable: PathBuf,
    model_directory: PathBuf,
    manifest_path: PathBuf,
    scratch_root: PathBuf,
    admitted_runtime: Mutex<Option<AdmittedSubjectMaskRuntime>>,
    scratch_sequence: AtomicU64,
}

#[derive(Debug)]
struct AdmittedSubjectMaskRuntime {
    installation: VerifiedSam2CoreMlInstallation,
    resident_session: Sam2CoreMlResidentSession,
}

#[derive(Debug)]
pub(crate) struct PreparedSubjectMaskInput {
    path: PathBuf,
}

impl PreparedSubjectMaskInput {
    pub(crate) fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for PreparedSubjectMaskInput {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

#[derive(Debug)]
pub(crate) struct SubjectMaskInvocation {
    pub(crate) request_id: String,
    pub(crate) promotion_id: String,
    pub(crate) generation: u64,
    pub(crate) photo_id: String,
    /// Display-encoded current edits with final photo geometry forced to
    /// identity. Prompt points and the generated raster therefore share the
    /// original-image normalized coordinate space used by Grade Node masks.
    pub(crate) original_space_input_jpeg: PathBuf,
    pub(crate) coordinate_extent: RasterExtent,
    pub(crate) points: Vec<MaskPromptPoint>,
}

#[derive(Debug, Clone)]
pub(crate) struct SubjectMaskRuntimeResources {
    hardware: HardwareProfile,
    policy: ResourcePolicy,
    route_estimate: ResourceEstimate,
}

impl SubjectMaskRuntime {
    pub(crate) fn new(
        executable: impl Into<PathBuf>,
        model_directory: impl Into<PathBuf>,
        manifest_path: impl Into<PathBuf>,
        scratch_root: impl Into<PathBuf>,
    ) -> Result<Self, SubjectMaskRuntimeError> {
        let scratch_root = scratch_root.into();
        fs::create_dir_all(&scratch_root).map_err(SubjectMaskRuntimeError::Scratch)?;
        Ok(Self {
            executable: executable.into(),
            model_directory: model_directory.into(),
            manifest_path: manifest_path.into(),
            scratch_root,
            admitted_runtime: Mutex::new(None),
            scratch_sequence: AtomicU64::new(0),
        })
    }

    pub(crate) fn prepare_input_jpeg(
        &self,
        bytes: &[u8],
    ) -> Result<PreparedSubjectMaskInput, SubjectMaskRuntimeError> {
        if bytes.is_empty() || bytes.len() > MAX_PROVIDER_INPUT_JPEG_BYTES {
            return Err(SubjectMaskRuntimeError::InputSize(bytes.len()));
        }
        let path = self.next_scratch_path("input", "jpg")?;
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .map_err(SubjectMaskRuntimeError::Scratch)?;
        if let Err(error) = file.write_all(bytes).and_then(|()| file.sync_all()) {
            let _ = fs::remove_file(&path);
            return Err(SubjectMaskRuntimeError::Scratch(error));
        }
        Ok(PreparedSubjectMaskInput { path })
    }

    pub(crate) fn stage(
        &self,
        store: &FilesystemDerivedRasterStore,
        invocation: SubjectMaskInvocation,
        cancellation: &CancellationToken,
    ) -> Result<DerivedRasterStageReceipt, SubjectMaskRuntimeError> {
        let resources = SubjectMaskRuntimeResources::current_macos()?;
        self.stage_with_resources(store, invocation, cancellation, &resources)
    }

    // This is one fail-closed admission transaction: splitting its linear
    // verify/admit/configure/stage sequence would obscure authority lifetimes.
    #[allow(clippy::too_many_lines)]
    fn stage_with_resources(
        &self,
        store: &FilesystemDerivedRasterStore,
        invocation: SubjectMaskInvocation,
        cancellation: &CancellationToken,
        resources: &SubjectMaskRuntimeResources,
    ) -> Result<DerivedRasterStageReceipt, SubjectMaskRuntimeError> {
        let input_bytes = fs::read(&invocation.original_space_input_jpeg)
            .map_err(|_| SubjectMaskRuntimeError::InputUnavailable)?;
        if input_bytes.is_empty() || input_bytes.len() > MAX_PROVIDER_INPUT_JPEG_BYTES {
            return Err(SubjectMaskRuntimeError::InputSize(input_bytes.len()));
        }
        let photo_id = PhotoId::from_str(&invocation.photo_id)
            .map_err(|_| SubjectMaskRuntimeError::InvalidPhotoId)?;
        let prompt = MaskPrompt::Points {
            points: invocation.points,
        };
        prompt
            .validate()
            .map_err(|_| SubjectMaskRuntimeError::InvalidPrompt)?;
        let input_content_hash = blake3::hash(&input_bytes).to_hex().to_string();
        let request = AiJobRequest {
            contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
            request_id: invocation.request_id.clone(),
            generation: invocation.generation,
            task: AiTaskKind::ProposeSubjectMask,
            target: ObservationTarget::Photo { photo_id },
            priority: TaskPriority::CurrentInput,
            privacy: PrivacyClass::Personal,
            inputs: vec![ArtifactReference {
                role: InputRole::DisplayProxy,
                content_hash: input_content_hash.clone(),
                byte_len: input_bytes.len() as u64,
                media_type: "image/jpeg".into(),
                privacy: PrivacyClass::Personal,
            }],
            parameters: AiTaskParameters::SubjectMask(SubjectMaskParameters {
                prompt,
                coordinate_space: MaskCoordinateSpace::Original,
                coordinate_extent: invocation.coordinate_extent,
                edge_refinement: UnitInterval::new(0.5).expect("constant edge refinement is valid"),
                maximum_candidates: 3,
            }),
            estimate: resources.route_estimate,
        };

        let mut runtime_guard = self
            .admitted_runtime
            .lock()
            .map_err(|_| SubjectMaskRuntimeError::StatePoisoned)?;
        if runtime_guard.is_none() {
            let installation = verify_sam2_coreml_installation(
                &self.executable,
                &self.model_directory,
                &self.manifest_path,
                cancellation,
            )?;
            let resident_session = Sam2CoreMlResidentSession::new(&installation);
            *runtime_guard = Some(AdmittedSubjectMaskRuntime {
                installation,
                resident_session,
            });
        }
        let admitted_runtime = runtime_guard
            .as_ref()
            .expect("admitted subject-mask runtime was inserted");
        let admission = admit_local_execution(
            LocalExecutionBinding {
                execution_id: format!("subject-mask-execution-{}", invocation.request_id),
                request,
                provider: ProviderIdentity {
                    provider_id: SAM2_COREML_PROVIDER_ID.into(),
                    adapter_revision: SAM2_COREML_ADAPTER_REVISION.into(),
                    execution_class: ProviderExecutionClass::LocalModel,
                },
                route_estimate: resources.route_estimate,
                fallback: FallbackDisclosure::Primary,
            },
            admitted_runtime.installation.manifest(),
            &resources.hardware,
            resources.policy,
            ModelAvailability {
                local: LocalModelAvailability::Installed {
                    artifact_set_blake3: admitted_runtime
                        .installation
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
            return Err(SubjectMaskRuntimeError::AdmissionDeferred(format!(
                "{blockers:?}"
            )));
        };
        let output_mask = self.next_scratch_path("proposal", "gray8")?;
        let provider = Sam2CoreMlSidecarProvider::new_resident(
            &admitted_runtime.installation,
            execution.plan_identity().clone(),
            &invocation.original_space_input_jpeg,
            input_content_hash,
            &output_mask,
            admitted_runtime.resident_session.clone(),
        )?;
        drop(runtime_guard);
        let lease = ExecutionLease::issue(
            format!("subject-mask-lease-{}", invocation.request_id),
            *execution,
        )?;
        execute_and_stage_derived_raster(
            store,
            invocation.promotion_id,
            lease,
            &provider,
            cancellation,
            provider.proposal_path(),
        )
        .map_err(SubjectMaskRuntimeError::Stage)
    }

    fn next_scratch_path(
        &self,
        role: &str,
        extension: &str,
    ) -> Result<PathBuf, SubjectMaskRuntimeError> {
        let token = self
            .scratch_sequence
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
                current.checked_add(1)
            })
            .map_err(|_| SubjectMaskRuntimeError::TokenExhausted)?
            + 1;
        Ok(self
            .scratch_root
            .join(format!("subject-mask-{role}-{token}.{extension}")))
    }
}

impl SubjectMaskRuntimeResources {
    fn current_macos() -> Result<Self, SubjectMaskRuntimeError> {
        if !cfg!(target_os = "macos") {
            return Err(SubjectMaskRuntimeError::PlatformUnsupported);
        }
        let total_system_ram_bytes = macos_total_memory_bytes()?;
        let logical_cpu_threads = u32::try_from(
            std::thread::available_parallelism()
                .map_err(|_| SubjectMaskRuntimeError::HardwareUnavailable)?
                .get(),
        )
        .map_err(|_| SubjectMaskRuntimeError::HardwareUnavailable)?;
        let available_system_ram_bytes = total_system_ram_bytes / 2;
        let maximum_ai_system_ram_bytes = total_system_ram_bytes / 2;
        let reserved_system_ram_bytes = (total_system_ram_bytes / 8).min(512 * MEBIBYTE);
        let cpu_threads = logical_cpu_threads.clamp(1, 2);
        Ok(Self {
            hardware: HardwareProfile {
                platform: Platform::MacOs,
                total_system_ram_bytes,
                available_system_ram_bytes,
                logical_cpu_threads,
                on_battery: false,
                backends: vec![ExecutionBackend {
                    id: "apple-coreml".into(),
                    kind: BackendKind::CoreMl,
                    available: true,
                    supported_precisions: vec![NumericPrecision::Float16],
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
            route_estimate: ResourceEstimate {
                peak_system_ram_bytes: GIBIBYTE,
                peak_device_memory_bytes: 0,
                cpu_threads,
                scratch_disk_bytes: 256 * MEBIBYTE,
                upload_bytes: 0,
                estimated_duration_ms: Some(1_500),
            },
        })
    }
}

fn macos_total_memory_bytes() -> Result<u64, SubjectMaskRuntimeError> {
    let output = Command::new("/usr/sbin/sysctl")
        .args(["-n", "hw.memsize"])
        .output()
        .map_err(|_| SubjectMaskRuntimeError::HardwareUnavailable)?;
    if !output.status.success() {
        return Err(SubjectMaskRuntimeError::HardwareUnavailable);
    }
    std::str::from_utf8(&output.stdout)
        .ok()
        .and_then(|value| value.trim().parse().ok())
        .filter(|value| *value > 0)
        .ok_or(SubjectMaskRuntimeError::HardwareUnavailable)
}

#[derive(Debug, Error)]
pub(crate) enum SubjectMaskRuntimeError {
    #[error("subject-mask runtime supports only macOS")]
    PlatformUnsupported,
    #[error("subject-mask hardware capacity is unavailable")]
    HardwareUnavailable,
    #[error("subject-mask input JPEG is unavailable")]
    InputUnavailable,
    #[error("subject-mask input JPEG size {0} is outside the bounded contract")]
    InputSize(usize),
    #[error("subject-mask photo identity is invalid")]
    InvalidPhotoId,
    #[error("subject-mask point prompt is invalid")]
    InvalidPrompt,
    #[error("subject-mask runtime state is poisoned")]
    StatePoisoned,
    #[error("subject-mask scratch output token space is exhausted")]
    TokenExhausted,
    #[error("subject-mask resource admission was deferred: {0}")]
    AdmissionDeferred(String),
    #[error("create subject-mask scratch directory")]
    Scratch(#[source] std::io::Error),
    #[error(transparent)]
    Verification(#[from] shadow_ai::Sam2CoreMlModelVerificationError),
    #[error(transparent)]
    RuntimeContract(#[from] shadow_ai::RuntimeContractError),
    #[error(transparent)]
    ProviderConfiguration(#[from] Sam2CoreMlProviderConfigurationError),
    #[error("stage subject-mask provider output")]
    Stage(#[source] DerivedRasterStageError),
}

#[cfg(test)]
mod tests;

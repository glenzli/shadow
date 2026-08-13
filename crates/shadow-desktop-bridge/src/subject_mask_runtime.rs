//! Desktop application adapter for one Infer Runtime SAM subject-mask result.
//!
//! Prepared JPEG rendering remains a desktop-session responsibility. This
//! owner constructs the product job, admits a typed Runtime result, and stages
//! its verified native-resolution bytes without applying them to a Recipe.

pub(crate) mod config;
pub(crate) mod geometry;

use std::{
    collections::BTreeSet,
    fs::{self, OpenOptions},
    io::Write as _,
    path::Path,
    path::PathBuf,
    str::FromStr,
    sync::atomic::{AtomicU64, Ordering},
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, ExecutionLease, ExecutionPlanIdentity,
    ExecutionRouteIdentity, FallbackDisclosure, InferRuntimeClient, InputRole, MaskPrompt,
    MaskPromptPoint, MaskSemantic, NumericPrecision, ObservationTarget, PrivacyClass,
    ProviderExecutionClass, ProviderIdentity, ProviderTerminal, RasterExtent, ResourceEstimate,
    RunPlan, RuntimeProgressReporter, RuntimeProvider, RuntimeUsage, SoftMaskArtifact,
    SoftMaskEncoding, SubjectMaskParameters, TaskPriority, UnitInterval,
    bind_local_service_execution,
};
use shadow_core::{
    DerivedRasterStageError, DerivedRasterStageReceipt, FilesystemDerivedRasterStore,
    execute_and_stage_derived_raster,
};
use shadow_domain::{MaskCoordinateSpace, PhotoId};
use thiserror::Error;

const MAX_PROVIDER_INPUT_JPEG_BYTES: usize = 64 * 1024 * 1024;
const SOFT_MASK_MEDIA_TYPE: &str = "application/x-shadow-soft-mask";
const SOFT_MASK_ENCODING_VERSION: u32 = 1;

#[derive(Debug)]
pub(crate) struct SubjectMaskRuntime {
    scratch_root: PathBuf,
    infer_base_url_override: Option<String>,
    infer_credential_file: PathBuf,
    scratch_sequence: AtomicU64,
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

impl SubjectMaskRuntime {
    pub(crate) fn new(
        scratch_root: impl Into<PathBuf>,
        infer_base_url_override: Option<String>,
        infer_credential_file: impl Into<PathBuf>,
    ) -> Result<Self, SubjectMaskRuntimeError> {
        let scratch_root = scratch_root.into();
        fs::create_dir_all(&scratch_root).map_err(SubjectMaskRuntimeError::Scratch)?;
        Ok(Self {
            scratch_root,
            infer_base_url_override,
            infer_credential_file: infer_credential_file.into(),
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
        self.stage_with_runtime(store, invocation, cancellation)
    }

    // Shadow owns input rendering, staging and durable promotion. Infer Runtime
    // owns the user-installed SAM model, scheduling and native worker lifecycle.
    #[allow(clippy::too_many_lines)]
    fn stage_with_runtime(
        &self,
        store: &FilesystemDerivedRasterStore,
        invocation: SubjectMaskInvocation,
        cancellation: &CancellationToken,
    ) -> Result<DerivedRasterStageReceipt, SubjectMaskRuntimeError> {
        let input_bytes = fs::read(&invocation.original_space_input_jpeg)
            .map_err(|_| SubjectMaskRuntimeError::InputUnavailable)?;
        if input_bytes.is_empty() || input_bytes.len() > MAX_PROVIDER_INPUT_JPEG_BYTES {
            return Err(SubjectMaskRuntimeError::InputSize(input_bytes.len()));
        }
        let photo_id = PhotoId::from_str(&invocation.photo_id)
            .map_err(|_| SubjectMaskRuntimeError::InvalidPhotoId)?;
        let prompt = MaskPrompt::Points {
            points: invocation.points.clone(),
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
            // Infer Runtime performs actual resource admission. This small
            // consumer plan only records the staged local result boundary.
            estimate: ResourceEstimate {
                cpu_threads: 1,
                ..ResourceEstimate::default()
            },
        };
        let output_mask = self.next_scratch_path("proposal", "gray8")?;
        let client = InferRuntimeClient::from_credential_file_with_discovery(
            self.infer_base_url_override.as_deref(),
            &self.infer_credential_file,
        )?;
        let source_revision = format!(
            "shadow:subject-mask/photo:{}/artifact:{input_content_hash}",
            invocation.photo_id
        );
        let Some(evidence) = client.segment_subject_soft_mask_cancellable(
            &input_bytes,
            &source_revision,
            match &request.parameters {
                AiTaskParameters::SubjectMask(parameters) => match &parameters.prompt {
                    MaskPrompt::Points { points } => points,
                    MaskPrompt::AutomaticSubject | MaskPrompt::Box { .. } => {
                        unreachable!("this runtime always requires points")
                    }
                },
                _ => unreachable!("subject-mask request has subject-mask parameters"),
            },
            cancellation,
        )?
        else {
            return Ok(cancelled_stage_receipt(&invocation));
        };
        if evidence.input_extent != invocation.coordinate_extent {
            return Err(SubjectMaskRuntimeError::RuntimeGeometryMismatch);
        }
        write_verified_soft_mask(&output_mask, &evidence.samples)?;
        let payload = soft_mask_payload(&evidence.samples, invocation.coordinate_extent)?;
        let (route, plan) = infer_route_and_plan(&evidence.provenance)?;
        let execution = bind_local_service_execution(
            format!("infer-subject-mask-execution-{}", invocation.request_id),
            request,
            route.clone(),
            &BTreeSet::from([AiCapability::SubjectMask]),
            plan.clone(),
            ResourceEstimate {
                cpu_threads: 1,
                ..ResourceEstimate::default()
            },
            FallbackDisclosure::Primary,
        )?;
        let provider = PreverifiedInferSubjectMaskProvider {
            route,
            plan: execution.plan_identity().clone(),
            payload,
        };
        let lease = ExecutionLease::issue(
            format!("subject-mask-lease-{}", invocation.request_id),
            execution,
        )?;
        execute_and_stage_derived_raster(
            store,
            invocation.promotion_id,
            lease,
            &provider,
            cancellation,
            &output_mask,
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

fn cancelled_stage_receipt(invocation: &SubjectMaskInvocation) -> DerivedRasterStageReceipt {
    DerivedRasterStageReceipt {
        request_id: invocation.request_id.clone(),
        generation: invocation.generation,
        usage: RuntimeUsage::default(),
        outcome: shadow_core::DerivedRasterStageOutcome::Cancelled,
    }
}

#[derive(Debug)]
struct PreverifiedInferSubjectMaskProvider {
    route: ExecutionRouteIdentity,
    plan: ExecutionPlanIdentity,
    payload: AiGeneratedPayload,
}

impl RuntimeProvider for PreverifiedInferSubjectMaskProvider {
    type Output = AiGeneratedPayload;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.plan
    }

    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::SubjectMask
    }

    fn execute(
        &self,
        _execution: &AdmittedExecution,
        cancellation: &CancellationToken,
        _progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        if cancellation.is_cancelled() {
            ProviderTerminal::Cancelled {
                usage: RuntimeUsage::default(),
            }
        } else {
            ProviderTerminal::Succeeded {
                output: self.payload.clone(),
                usage: RuntimeUsage::default(),
            }
        }
    }
}

fn write_verified_soft_mask(path: &Path, samples: &[u8]) -> Result<(), SubjectMaskRuntimeError> {
    if samples.len() != 256 * 256 {
        return Err(SubjectMaskRuntimeError::RuntimeMaskMalformed);
    }
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(SubjectMaskRuntimeError::Scratch)?;
    if let Err(error) = file.write_all(samples).and_then(|()| file.sync_all()) {
        let _ = fs::remove_file(path);
        return Err(SubjectMaskRuntimeError::Scratch(error));
    }
    Ok(())
}

fn soft_mask_payload(
    samples: &[u8],
    coordinate_extent: RasterExtent,
) -> Result<AiGeneratedPayload, SubjectMaskRuntimeError> {
    let artifact = shadow_ai::GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        blake3::hash(samples).to_hex().to_string(),
        u64::try_from(samples.len()).map_err(|_| SubjectMaskRuntimeError::RuntimeMaskMalformed)?,
        SOFT_MASK_MEDIA_TYPE.into(),
        SOFT_MASK_ENCODING_VERSION,
    )
    .map_err(|_| SubjectMaskRuntimeError::RuntimeMaskMalformed)?;
    Ok(AiGeneratedPayload::SoftMask(SoftMaskArtifact {
        artifact,
        raster_extent: RasterExtent::new(256, 256)
            .map_err(|_| SubjectMaskRuntimeError::RuntimeMaskMalformed)?,
        coordinate_extent,
        coordinate_space: MaskCoordinateSpace::Original,
        encoding: SoftMaskEncoding::Gray8Unorm,
        semantic: MaskSemantic::UserPrompt,
    }))
}

fn infer_route_and_plan(
    provenance: &shadow_ai::VisionProvenance,
) -> Result<(ExecutionRouteIdentity, RunPlan), SubjectMaskRuntimeError> {
    if provenance.execution_provider_fallback_reason.is_some() {
        return Err(SubjectMaskRuntimeError::RuntimeFallbackDisclosed);
    }
    let backend_kind = match provenance
        .actual_execution_provider
        .to_ascii_lowercase()
        .as_str()
    {
        value if value.contains("coreml") => BackendKind::CoreMl,
        value if value.contains("cpu") => BackendKind::Cpu,
        _ => return Err(SubjectMaskRuntimeError::RuntimeProvenanceMalformed),
    };
    let precision = match provenance.precision.to_ascii_lowercase().as_str() {
        "float16" | "fp16" | "f16" => NumericPrecision::Float16,
        "float32" | "fp32" | "f32" => NumericPrecision::Float32,
        _ => return Err(SubjectMaskRuntimeError::RuntimeProvenanceMalformed),
    };
    let route = ExecutionRouteIdentity {
        contract_version: shadow_ai::EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: provenance.provider.clone(),
            adapter_revision: provenance.postprocessing_identity.clone(),
            execution_class: ProviderExecutionClass::LocalService,
        },
        model: AdmittedModelIdentity::RemoteService {
            service_revision: provenance.runtime.clone(),
            model_id: provenance.model_build.clone(),
            model_revision: provenance.artifact_sha256.clone(),
            api_contract_revision: shadow_ai::INFER_SUBJECT_MASK_CAPABILITY.into(),
        },
    };
    Ok((
        route,
        RunPlan {
            backend_id: format!("infer-runtime/{}", provenance.deployment),
            backend_kind,
            precision,
            cpu_threads: 1,
            reserved_system_ram_bytes: 0,
            reserved_device_memory_bytes: 0,
        },
    ))
}

#[derive(Debug, Error)]
pub(crate) enum SubjectMaskRuntimeError {
    #[error("subject-mask input JPEG is unavailable")]
    InputUnavailable,
    #[error("subject-mask input JPEG size {0} is outside the bounded contract")]
    InputSize(usize),
    #[error("subject-mask photo identity is invalid")]
    InvalidPhotoId,
    #[error("subject-mask point prompt is invalid")]
    InvalidPrompt,
    #[error("subject-mask scratch output token space is exhausted")]
    TokenExhausted,
    #[error("Infer Runtime returned a subject mask for a different input geometry")]
    RuntimeGeometryMismatch,
    #[error("Infer Runtime returned a malformed subject probability mask")]
    RuntimeMaskMalformed,
    #[error("Infer Runtime disclosed a forbidden execution-provider fallback")]
    RuntimeFallbackDisclosed,
    #[error("Infer Runtime returned malformed subject-mask provenance")]
    RuntimeProvenanceMalformed,
    #[error("create subject-mask scratch directory")]
    Scratch(#[source] std::io::Error),
    #[error(transparent)]
    RuntimeContract(#[from] shadow_ai::RuntimeContractError),
    #[error(transparent)]
    Infer(#[from] shadow_ai::InferRuntimeClientError),
    #[error("stage subject-mask provider output")]
    Stage(#[source] DerivedRasterStageError),
}

#[cfg(test)]
mod tests;

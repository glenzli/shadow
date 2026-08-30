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
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, DetectedFace, ExecutionLease,
    ExecutionPlanIdentity, ExecutionRouteIdentity, FaceAnalysisProvider, FallbackDisclosure,
    InferRuntimeClient, InputRole, MaskPointPolarity, MaskPrompt, MaskPromptPoint, MaskSemantic,
    NumericPrecision, ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity,
    ProviderTerminal, RasterExtent, ResourceEstimate, RunPlan, RuntimeProgressReporter,
    RuntimeProvider, RuntimeUsage, SoftMaskArtifact, SoftMaskEncoding, SubjectMaskParameters,
    TaskPriority, UnitInterval, bind_local_service_execution,
};
use shadow_core::{
    DerivedRasterStageError, DerivedRasterStageReceipt, FilesystemDerivedRasterStore,
    execute_and_stage_derived_raster,
};
use shadow_domain::{MaskCoordinateSpace, PhotoId, SemanticMaskIntent};
use thiserror::Error;

use crate::subject_mask_people::{
    FaceRegionSet, ParsedSubjectMaskPerson, SubjectMaskPersonCandidate, compose_face_region_mask,
};
use crate::subject_mask_service::PreparedSubjectMaskInput;

const MAX_PROVIDER_INPUT_JPEG_BYTES: usize = 64 * 1024 * 1024;
const SOFT_MASK_MEDIA_TYPE: &str = "application/x-shadow-soft-mask";
const SOFT_MASK_ENCODING_VERSION: u32 = 1;
const INFER_FACE_PARSING_CAPABILITY: &str = "infer.vision.face-parsing@20260813.1";

#[derive(Debug)]
pub(crate) struct SubjectMaskRuntime {
    scratch_root: PathBuf,
    infer_base_url_override: Option<String>,
    infer_credential_file: PathBuf,
    scratch_sequence: AtomicU64,
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
    pub(crate) original_space_input_jpeg: Arc<[u8]>,
    pub(crate) original_space_input_content_hash: String,
    pub(crate) coordinate_extent: RasterExtent,
    pub(crate) selection: SubjectMaskSelection,
}

#[derive(Debug)]
pub(crate) struct SemanticMaskInvocation {
    pub(crate) request_id: String,
    pub(crate) promotion_id: String,
    pub(crate) generation: u64,
    pub(crate) photo_id: String,
    pub(crate) original_space_input: PreparedSubjectMaskInput,
    pub(crate) intent: SemanticMaskIntent,
}

#[derive(Debug)]
pub(crate) enum SemanticMaskStageOutcome {
    Staged(DerivedRasterStageReceipt),
    ProviderUnavailable(DerivedRasterStageReceipt),
    ProviderFailed(DerivedRasterStageReceipt),
    NotFound,
    Cancelled,
    Unavailable(SubjectMaskRuntimeError),
}

#[derive(Debug, Clone)]
pub(crate) enum SubjectMaskSelection {
    PromptedSubject {
        points: Vec<MaskPromptPoint>,
    },
    FaceRegions {
        person: SubjectMaskPersonCandidate,
        regions: FaceRegionSet,
        parsed: Arc<ParsedSubjectMaskPerson>,
    },
    SemanticQuery {
        intent: SemanticMaskIntent,
    },
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

    pub(crate) fn stage(
        &self,
        store: &FilesystemDerivedRasterStore,
        invocation: SubjectMaskInvocation,
        cancellation: &CancellationToken,
    ) -> Result<DerivedRasterStageReceipt, SubjectMaskRuntimeError> {
        self.stage_with_runtime(store, invocation, cancellation)
    }

    /// Grounds one semantic intent and stages its SAM-derived raster without
    /// publishing durable Recipe state.
    pub(crate) fn stage_semantic_intent(
        &self,
        store: &FilesystemDerivedRasterStore,
        invocation: SemanticMaskInvocation,
        cancellation: &CancellationToken,
    ) -> SemanticMaskStageOutcome {
        let invocation = SubjectMaskInvocation {
            request_id: invocation.request_id,
            promotion_id: invocation.promotion_id,
            generation: invocation.generation,
            photo_id: invocation.photo_id,
            original_space_input_jpeg: invocation.original_space_input.bytes,
            original_space_input_content_hash: invocation.original_space_input.content_hash,
            coordinate_extent: invocation.original_space_input.coordinate_extent,
            selection: SubjectMaskSelection::SemanticQuery {
                intent: invocation.intent,
            },
        };
        Self::classify_semantic_stage(self.stage_with_runtime(store, invocation, cancellation))
    }

    fn classify_semantic_stage(
        result: Result<DerivedRasterStageReceipt, SubjectMaskRuntimeError>,
    ) -> SemanticMaskStageOutcome {
        match result {
            Ok(receipt)
                if matches!(
                    receipt.outcome,
                    shadow_core::DerivedRasterStageOutcome::Unavailable { .. }
                ) =>
            {
                SemanticMaskStageOutcome::ProviderUnavailable(receipt)
            }
            Ok(receipt)
                if matches!(
                    receipt.outcome,
                    shadow_core::DerivedRasterStageOutcome::Failed { .. }
                ) =>
            {
                SemanticMaskStageOutcome::ProviderFailed(receipt)
            }
            Ok(receipt)
                if matches!(
                    receipt.outcome,
                    shadow_core::DerivedRasterStageOutcome::Cancelled
                ) =>
            {
                SemanticMaskStageOutcome::Cancelled
            }
            Ok(receipt) => SemanticMaskStageOutcome::Staged(receipt),
            Err(SubjectMaskRuntimeError::SemanticQueryNotFound) => {
                SemanticMaskStageOutcome::NotFound
            }
            Err(error) => SemanticMaskStageOutcome::Unavailable(error),
        }
    }

    pub(crate) fn detect_people(
        &self,
        input_jpeg: &[u8],
        input_content_hash: &str,
        coordinate_extent: RasterExtent,
        cancellation: &CancellationToken,
    ) -> Result<Option<Vec<DetectedFace>>, SubjectMaskRuntimeError> {
        validate_input_jpeg(input_jpeg)?;
        if cancellation.is_cancelled() {
            return Ok(None);
        }
        let detections = self.client()?.detect_faces(
            input_jpeg,
            "image/jpeg",
            &input_source_revision(input_content_hash),
        )?;
        if detections.width != coordinate_extent.width
            || detections.height != coordinate_extent.height
        {
            return Err(SubjectMaskRuntimeError::RuntimeGeometryMismatch);
        }
        if cancellation.is_cancelled() {
            Ok(None)
        } else {
            Ok(Some(detections.detections))
        }
    }

    pub(crate) fn parse_person(
        &self,
        input_jpeg: &[u8],
        input_content_hash: &str,
        coordinate_extent: RasterExtent,
        person: &SubjectMaskPersonCandidate,
        cancellation: &CancellationToken,
    ) -> Result<Option<ParsedSubjectMaskPerson>, SubjectMaskRuntimeError> {
        validate_input_jpeg(input_jpeg)?;
        if cancellation.is_cancelled() {
            return Ok(None);
        }
        let parsed = self.client()?.parse_face(
            input_jpeg,
            "image/jpeg",
            &input_source_revision(input_content_hash),
            person.bounding_box,
        )?;
        if parsed.width != coordinate_extent.width || parsed.height != coordinate_extent.height {
            return Err(SubjectMaskRuntimeError::RuntimeGeometryMismatch);
        }
        if cancellation.is_cancelled() {
            Ok(None)
        } else {
            Ok(Some(ParsedSubjectMaskPerson::new(
                parsed.labels,
                coordinate_extent,
                parsed.provenance,
            )?))
        }
    }

    fn client(&self) -> Result<InferRuntimeClient, SubjectMaskRuntimeError> {
        Ok(InferRuntimeClient::from_credential_file_with_discovery(
            self.infer_base_url_override.as_deref(),
            &self.infer_credential_file,
        )?)
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
        let input_bytes = invocation.original_space_input_jpeg.as_ref();
        validate_input_jpeg(input_bytes)?;
        let photo_id = PhotoId::from_str(&invocation.photo_id)
            .map_err(|_| SubjectMaskRuntimeError::InvalidPhotoId)?;
        let prompt = match &invocation.selection {
            SubjectMaskSelection::PromptedSubject { points } => MaskPrompt::Points {
                points: points.clone(),
            },
            SubjectMaskSelection::FaceRegions { person, .. } => MaskPrompt::Points {
                points: vec![MaskPromptPoint {
                    x: UnitInterval::new(
                        ((person.bounding_box.x + person.bounding_box.width * 0.5)
                            / invocation.coordinate_extent.width as f32)
                            as f64,
                    )
                    .map_err(|_| SubjectMaskRuntimeError::InvalidPrompt)?,
                    y: UnitInterval::new(
                        ((person.bounding_box.y + person.bounding_box.height * 0.5)
                            / invocation.coordinate_extent.height as f32)
                            as f64,
                    )
                    .map_err(|_| SubjectMaskRuntimeError::InvalidPrompt)?,
                    polarity: MaskPointPolarity::Foreground,
                }],
            },
            SubjectMaskSelection::SemanticQuery { .. } => MaskPrompt::AutomaticSubject,
        };
        prompt
            .validate()
            .map_err(|_| SubjectMaskRuntimeError::InvalidPrompt)?;
        let input_content_hash = &invocation.original_space_input_content_hash;
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
        let (samples, raster_extent, provenance, api_contract) = match &invocation.selection {
            SubjectMaskSelection::PromptedSubject { .. } => {
                let client = self.client()?;
                let Some(evidence) = client.segment_subject_soft_mask_cancellable(
                    input_bytes,
                    &input_source_revision(input_content_hash),
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
                (
                    evidence.samples,
                    RasterExtent::new(256, 256)
                        .map_err(|_| SubjectMaskRuntimeError::RuntimeMaskMalformed)?,
                    evidence.provenance,
                    shadow_ai::INFER_SUBJECT_MASK_CAPABILITY,
                )
            }
            SubjectMaskSelection::FaceRegions {
                regions, parsed, ..
            } => {
                let samples = compose_face_region_mask(parsed, *regions)?;
                (
                    samples,
                    invocation.coordinate_extent,
                    parsed.provenance.clone(),
                    INFER_FACE_PARSING_CAPABILITY,
                )
            }
            SubjectMaskSelection::SemanticQuery { intent } => {
                let client = self.client()?;
                let source_revision = input_source_revision(input_content_hash);
                let Some(grounding) = client.ground_semantics_cancellable(
                    input_bytes,
                    &source_revision,
                    intent.query(),
                    intent.query_revision(),
                    intent.maximum_regions(),
                    f32::from(intent.score_threshold_percent()) / 100.0,
                    cancellation,
                )?
                else {
                    return Ok(cancelled_stage_receipt(&invocation));
                };
                if grounding.input_extent != invocation.coordinate_extent {
                    return Err(SubjectMaskRuntimeError::RuntimeGeometryMismatch);
                }
                if grounding.regions.is_empty() {
                    return Err(SubjectMaskRuntimeError::SemanticQueryNotFound);
                }
                let mut merged = vec![0_u8; 256 * 256];
                let mut sam_provenance = None;
                for region in grounding.regions {
                    let Some(evidence) = client.segment_subject_box_soft_mask_cancellable(
                        input_bytes,
                        &source_revision,
                        region,
                        cancellation,
                    )?
                    else {
                        return Ok(cancelled_stage_receipt(&invocation));
                    };
                    if evidence.input_extent != invocation.coordinate_extent
                        || evidence.samples.len() != merged.len()
                    {
                        return Err(SubjectMaskRuntimeError::RuntimeGeometryMismatch);
                    }
                    for (target, sample) in merged.iter_mut().zip(evidence.samples) {
                        *target = (*target).max(sample);
                    }
                    sam_provenance = Some(evidence.provenance);
                }
                (
                    merged,
                    RasterExtent::new(256, 256)
                        .map_err(|_| SubjectMaskRuntimeError::RuntimeMaskMalformed)?,
                    sam_provenance.ok_or(SubjectMaskRuntimeError::SemanticQueryNotFound)?,
                    shadow_ai::INFER_SUBJECT_MASK_CAPABILITY,
                )
            }
        };
        if cancellation.is_cancelled() {
            return Ok(cancelled_stage_receipt(&invocation));
        }
        write_verified_soft_mask(&output_mask, &samples, raster_extent)?;
        let payload = soft_mask_payload(&samples, raster_extent, invocation.coordinate_extent)?;
        let (route, plan) = infer_route_and_plan(&provenance, api_contract)?;
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

fn input_source_revision(input_content_hash: &str) -> String {
    format!("shadow:subject-mask/artifact:{input_content_hash}")
}

fn validate_input_jpeg(bytes: &[u8]) -> Result<(), SubjectMaskRuntimeError> {
    if bytes.is_empty() || bytes.len() > MAX_PROVIDER_INPUT_JPEG_BYTES {
        Err(SubjectMaskRuntimeError::InputSize(bytes.len()))
    } else {
        Ok(())
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

fn write_verified_soft_mask(
    path: &Path,
    samples: &[u8],
    raster_extent: RasterExtent,
) -> Result<(), SubjectMaskRuntimeError> {
    let expected = u64::from(raster_extent.width)
        .checked_mul(u64::from(raster_extent.height))
        .and_then(|value| usize::try_from(value).ok())
        .ok_or(SubjectMaskRuntimeError::RuntimeMaskMalformed)?;
    if samples.len() != expected {
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
    raster_extent: RasterExtent,
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
        raster_extent,
        coordinate_extent,
        coordinate_space: MaskCoordinateSpace::Original,
        encoding: SoftMaskEncoding::Gray8Unorm,
        semantic: MaskSemantic::UserPrompt,
    }))
}

fn infer_route_and_plan(
    provenance: &shadow_ai::VisionProvenance,
    api_contract_revision: &str,
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
            api_contract_revision: api_contract_revision.into(),
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
    #[error("semantic query did not match a visible region")]
    SemanticQueryNotFound,
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
    #[error(transparent)]
    People(#[from] crate::subject_mask_people::SubjectMaskPeopleError),
    #[error("stage subject-mask provider output")]
    Stage(#[source] DerivedRasterStageError),
}

#[cfg(test)]
mod tests;

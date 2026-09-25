//! Infer Runtime adapter for one staged image-completion proposal.
//!
//! Shadow prepares the crop and exact Gray8 selection. Infer Runtime owns
//! model execution. This owner admits the returned RGB raster, restores the
//! user mask as alpha, and stages immutable RGBA8 bytes without Recipe write
//! authority.

use std::{
    collections::BTreeSet,
    fs::{self, OpenOptions},
    io::Write as _,
    path::{Path, PathBuf},
    str::FromStr,
    sync::atomic::{AtomicU64, Ordering},
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, ExecutionLease, ExecutionPlanIdentity,
    ExecutionRouteIdentity, FallbackDisclosure, GeneratedArtifactReference,
    ImageCompletionParameters, ImageCompletionPatchArtifact, InferRuntimeClient, InputRole,
    NumericPrecision, ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity,
    ProviderTerminal, RasterExtent, ResourceEstimate, RunPlan, RuntimeProgressReporter,
    RuntimeProvider, RuntimeUsage, TaskPriority, VisionProvenance, bind_local_service_execution,
};
use shadow_core::{
    DerivedRasterStageError, DerivedRasterStageReceipt, FilesystemDerivedRasterStore,
    SHADOW_RGBA8_ENCODING_VERSION, SHADOW_RGBA8_MEDIA_TYPE, execute_and_stage_derived_raster,
};
use shadow_domain::PhotoId;
use thiserror::Error;

const MAX_INPUT_PNG_BYTES: usize = 64 * 1_024 * 1_024;

#[derive(Debug)]
pub(crate) struct ImageCompletionRuntime {
    scratch_root: PathBuf,
    infer_base_url_override: Option<String>,
    infer_credential_file: PathBuf,
    scratch_sequence: AtomicU64,
}

#[derive(Debug)]
pub(crate) struct ImageCompletionInvocation {
    pub(crate) request_id: String,
    pub(crate) promotion_id: String,
    pub(crate) generation: u64,
    pub(crate) photo_id: String,
    pub(crate) prepared_crop_png: Vec<u8>,
    pub(crate) prepared_mask_png: Vec<u8>,
    pub(crate) prepared_mask_gray8: Vec<u8>,
    pub(crate) coordinate_extent: RasterExtent,
    pub(crate) source_recipe_blake3: String,
    pub(crate) mask_revision: String,
    pub(crate) scene_referred_input: bool,
}

impl ImageCompletionRuntime {
    pub(crate) fn new(
        scratch_root: impl Into<PathBuf>,
        infer_base_url_override: Option<String>,
        infer_credential_file: impl Into<PathBuf>,
    ) -> Result<Self, ImageCompletionRuntimeError> {
        let scratch_root = scratch_root.into();
        fs::create_dir_all(&scratch_root).map_err(ImageCompletionRuntimeError::Scratch)?;
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
        invocation: ImageCompletionInvocation,
        cancellation: &CancellationToken,
    ) -> Result<DerivedRasterStageReceipt, ImageCompletionRuntimeError> {
        validate_input(&invocation)?;
        let photo_id = PhotoId::from_str(&invocation.photo_id)
            .map_err(|_| ImageCompletionRuntimeError::InvalidPhotoId)?;
        let request = AiJobRequest {
            contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
            request_id: invocation.request_id.clone(),
            generation: invocation.generation,
            task: AiTaskKind::GenerateInpaintPatch,
            target: ObservationTarget::Photo { photo_id },
            priority: TaskPriority::CurrentInput,
            privacy: PrivacyClass::Personal,
            inputs: vec![
                ArtifactReference {
                    role: InputRole::CurrentRenderedCrop,
                    content_hash: blake3::hash(&invocation.prepared_crop_png)
                        .to_hex()
                        .to_string(),
                    byte_len: invocation.prepared_crop_png.len() as u64,
                    media_type: "image/png".into(),
                    privacy: PrivacyClass::Personal,
                },
                ArtifactReference {
                    role: InputRole::Mask,
                    content_hash: invocation.mask_revision.clone(),
                    byte_len: invocation.prepared_mask_png.len() as u64,
                    media_type: "image/png".into(),
                    privacy: PrivacyClass::Personal,
                },
            ],
            parameters: AiTaskParameters::ImageCompletion(ImageCompletionParameters {
                coordinate_extent: invocation.coordinate_extent,
                source_recipe_blake3: invocation.source_recipe_blake3.clone(),
                mask_revision: invocation.mask_revision.clone(),
            }),
            estimate: ResourceEstimate {
                cpu_threads: 1,
                ..ResourceEstimate::default()
            },
        };
        let source_revision = format!(
            "shadow:image-completion/source:{}",
            blake3::hash(&invocation.prepared_crop_png).to_hex()
        );
        let client = InferRuntimeClient::from_credential_file_with_discovery(
            self.infer_base_url_override.as_deref(),
            &self.infer_credential_file,
        )?;
        let Some(evidence) = client.complete_image_cancellable(
            &invocation.prepared_crop_png,
            &invocation.prepared_mask_png,
            &source_revision,
            &invocation.mask_revision,
            cancellation,
        )?
        else {
            return Ok(cancelled_stage_receipt(&invocation));
        };
        if cancellation.is_cancelled() {
            return Ok(cancelled_stage_receipt(&invocation));
        }
        let pixel_count = usize::try_from(
            u64::from(evidence.raster_extent.width) * u64::from(evidence.raster_extent.height),
        )
        .map_err(|_| ImageCompletionRuntimeError::MalformedRuntimeRaster)?;
        if evidence.rgb8.len() != pixel_count * 3
            || invocation.prepared_mask_gray8.len() != pixel_count
        {
            return Err(ImageCompletionRuntimeError::MalformedRuntimeRaster);
        }
        let alpha =
            feather_completion_alpha(&invocation.prepared_mask_gray8, evidence.raster_extent);
        let mut rgba8 = Vec::with_capacity(pixel_count * 4);
        for (rgb, alpha) in evidence.rgb8.chunks_exact(3).zip(&alpha) {
            // Infer Runtime sees Shadow's display-referred preview. The accepted
            // patch is applied to scene-linear working RGB before Grade Nodes,
            // so undo the neutral display curve before storing its sRGB8 bytes.
            // The native compositor decodes these bytes back to linear RGB.
            let encoded =
                encode_completion_rgb([rgb[0], rgb[1], rgb[2]], invocation.scene_referred_input);
            rgba8.extend_from_slice(&encoded);
            rgba8.push(*alpha);
        }
        let output = self.next_scratch_path("proposal", "rgba8")?;
        write_verified_patch(&output, &rgba8)?;
        let artifact = GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Blake3_256,
            blake3::hash(&rgba8).to_hex().to_string(),
            rgba8.len() as u64,
            SHADOW_RGBA8_MEDIA_TYPE.into(),
            SHADOW_RGBA8_ENCODING_VERSION,
        )?;
        let payload = AiGeneratedPayload::ImageCompletionPatch(ImageCompletionPatchArtifact {
            artifact,
            raster_extent: evidence.raster_extent,
            coordinate_extent: invocation.coordinate_extent,
            source_recipe_blake3: invocation.source_recipe_blake3,
            provider: evidence.provenance.provider.clone(),
            deployment: evidence.provenance.deployment.clone(),
            model_build: evidence.provenance.model_build.clone(),
            postprocessing_identity: evidence.provenance.postprocessing_identity.clone(),
            api_contract_revision: shadow_ai::INFER_IMAGE_COMPLETION_CAPABILITY.into(),
            actual_execution_provider: evidence.provenance.actual_execution_provider.clone(),
        });
        payload.validate_for(AiTaskKind::GenerateInpaintPatch)?;
        let (route, plan) = infer_route_and_plan(&evidence.provenance)?;
        let execution = bind_local_service_execution(
            format!("infer-image-completion-execution-{}", invocation.request_id),
            request,
            route.clone(),
            &BTreeSet::from([AiCapability::InpaintPatch]),
            plan.clone(),
            ResourceEstimate {
                cpu_threads: 1,
                ..ResourceEstimate::default()
            },
            FallbackDisclosure::Primary,
        )?;
        let provider = PreverifiedInferImageCompletionProvider {
            route,
            plan: execution.plan_identity().clone(),
            payload,
        };
        let lease = ExecutionLease::issue(
            format!("image-completion-lease-{}", invocation.request_id),
            execution,
        )?;
        execute_and_stage_derived_raster(
            store,
            invocation.promotion_id,
            lease,
            &provider,
            cancellation,
            &output,
        )
        .map_err(ImageCompletionRuntimeError::Stage)
    }

    fn next_scratch_path(
        &self,
        role: &str,
        extension: &str,
    ) -> Result<PathBuf, ImageCompletionRuntimeError> {
        let token = self
            .scratch_sequence
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
                current.checked_add(1)
            })
            .map_err(|_| ImageCompletionRuntimeError::TokenExhausted)?
            + 1;
        Ok(self
            .scratch_root
            .join(format!("image-completion-{role}-{token}.{extension}")))
    }
}

fn encode_completion_rgb(rgb: [u8; 3], scene_referred_input: bool) -> [u8; 3] {
    if scene_referred_input {
        display_rgb_to_scene_encoded(rgb)
    } else {
        rgb
    }
}

/// Convert model RGB from Shadow's SDR presentation space to the scene-linear
/// sRGB8 encoding expected by the pre-grade completion compositor. This is
/// the inverse of the neutral luminance curve in native display_output.cpp;
/// gamut compression and 8-bit highlight clipping cannot be reversed.
fn display_rgb_to_scene_encoded(rgb: [u8; 3]) -> [u8; 3] {
    let display = rgb.map(|value| {
        let encoded = f64::from(value) / 255.0;
        if encoded <= 0.04045 {
            encoded / 12.92
        } else {
            ((encoded + 0.055) / 1.055).powf(2.4)
        }
    });
    let luminance = display[0] * 0.2126 + display[1] * 0.7152 + display[2] * 0.0722;
    if luminance <= 0.0 {
        return rgb;
    }
    let rec709_encoded = srgb_encode(luminance);
    let shouldered = if rec709_encoded < 0.081 {
        rec709_encoded / 4.5
    } else {
        ((rec709_encoded + 0.099) / 1.099).powf(1.0 / 0.45)
    };
    let scene_luminance = if shouldered <= 0.75 {
        shouldered
    } else {
        let shoulder = shouldered - 0.75;
        0.75 + 0.25 * shoulder / (0.25 - shoulder).max(1.0e-9)
    };
    let gain = scene_luminance / luminance;
    display.map(|channel| {
        let encoded = srgb_encode((channel * gain).clamp(0.0, 1.0));
        (encoded * 255.0).round().clamp(0.0, 255.0) as u8
    })
}

fn srgb_encode(linear: f64) -> f64 {
    if linear <= 0.003_130_8 {
        12.92 * linear
    } else {
        1.055 * linear.powf(1.0 / 2.4) - 0.055
    }
}

/// Feather inward only: unselected and erased pixels always remain untouched.
/// The model still receives the exact binary selection; this is a one-time
/// candidate publication step, shared by preview, detail, and export.
fn feather_completion_alpha(mask: &[u8], extent: RasterExtent) -> Vec<u8> {
    let image = image::GrayImage::from_raw(extent.width, extent.height, mask.to_vec())
        .expect("validated completion mask dimensions");
    let softened = image::imageops::blur(&image, 3.0);
    mask.iter()
        .zip(softened.as_raw())
        .map(|(original, blurred)| {
            if *original == 0 {
                0
            } else {
                u8::try_from(u16::from(blurred.saturating_sub(128)) * 255 / 127)
                    .expect("bounded feather alpha")
            }
        })
        .collect()
}

fn validate_input(
    invocation: &ImageCompletionInvocation,
) -> Result<(), ImageCompletionRuntimeError> {
    if invocation.prepared_crop_png.is_empty()
        || invocation.prepared_crop_png.len() > MAX_INPUT_PNG_BYTES
        || invocation.prepared_mask_png.is_empty()
        || invocation.prepared_mask_png.len() > MAX_INPUT_PNG_BYTES
        || invocation.prepared_mask_gray8.is_empty()
        || invocation.source_recipe_blake3.len() != 64
        || invocation.mask_revision.len() != 64
    {
        return Err(ImageCompletionRuntimeError::InvalidInput);
    }
    Ok(())
}

fn write_verified_patch(path: &Path, bytes: &[u8]) -> Result<(), ImageCompletionRuntimeError> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(ImageCompletionRuntimeError::Scratch)?;
    if let Err(error) = file.write_all(bytes).and_then(|()| file.sync_all()) {
        let _ = fs::remove_file(path);
        return Err(ImageCompletionRuntimeError::Scratch(error));
    }
    Ok(())
}

fn cancelled_stage_receipt(invocation: &ImageCompletionInvocation) -> DerivedRasterStageReceipt {
    DerivedRasterStageReceipt {
        request_id: invocation.request_id.clone(),
        generation: invocation.generation,
        usage: RuntimeUsage::default(),
        outcome: shadow_core::DerivedRasterStageOutcome::Cancelled,
    }
}

#[derive(Debug)]
struct PreverifiedInferImageCompletionProvider {
    route: ExecutionRouteIdentity,
    plan: ExecutionPlanIdentity,
    payload: AiGeneratedPayload,
}

impl RuntimeProvider for PreverifiedInferImageCompletionProvider {
    type Output = AiGeneratedPayload;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }
    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.plan
    }
    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::InpaintPatch
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

fn infer_route_and_plan(
    provenance: &VisionProvenance,
) -> Result<(ExecutionRouteIdentity, RunPlan), ImageCompletionRuntimeError> {
    if provenance.execution_provider_fallback_reason.is_some() {
        return Err(ImageCompletionRuntimeError::RuntimeFallbackDisclosed);
    }
    let backend_kind = match provenance
        .actual_execution_provider
        .to_ascii_lowercase()
        .as_str()
    {
        value if value.contains("coreml") => BackendKind::CoreMl,
        value if value.contains("cpu") => BackendKind::Cpu,
        _ => return Err(ImageCompletionRuntimeError::RuntimeProvenanceMalformed),
    };
    let precision = match provenance.precision.to_ascii_lowercase().as_str() {
        "float16" | "fp16" | "f16" => NumericPrecision::Float16,
        "float32" | "fp32" | "f32" => NumericPrecision::Float32,
        _ => return Err(ImageCompletionRuntimeError::RuntimeProvenanceMalformed),
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
            api_contract_revision: shadow_ai::INFER_IMAGE_COMPLETION_CAPABILITY.into(),
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
pub(crate) enum ImageCompletionRuntimeError {
    #[error("image-completion input violates its bounded contract")]
    InvalidInput,
    #[error("image-completion photo identity is invalid")]
    InvalidPhotoId,
    #[error("image-completion scratch token space is exhausted")]
    TokenExhausted,
    #[error("Infer Runtime returned a malformed image-completion raster")]
    MalformedRuntimeRaster,
    #[error("Infer Runtime disclosed a forbidden execution-provider fallback")]
    RuntimeFallbackDisclosed,
    #[error("Infer Runtime returned malformed image-completion provenance")]
    RuntimeProvenanceMalformed,
    #[error("write image-completion scratch output")]
    Scratch(#[source] std::io::Error),
    #[error(transparent)]
    RuntimeContract(#[from] shadow_ai::RuntimeContractError),
    #[error(transparent)]
    Artifact(#[from] shadow_ai::AiArtifactContractError),
    #[error(transparent)]
    Infer(#[from] shadow_ai::InferRuntimeClientError),
    #[error("stage image-completion provider output")]
    Stage(#[source] DerivedRasterStageError),
}

#[cfg(test)]
mod tests;

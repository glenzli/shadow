//! Shadow product adapters over the official Infer Runtime Consumer SDK.
//!
//! The SDK is the sole owner of Infra Discovery, Core and Capability contract
//! negotiation, managed credential loading, loopback HTTP policy, retries,
//! headers, and public error decoding. Shadow retains only its product-facing
//! synchronous provider traits, input staging, strict evidence admission, and
//! stale/cache/publish policy in callers.

mod image_completion;
mod image_understanding;
mod raw_foundation;
mod semantic;
mod semantic_grounding;
mod subject_mask;

use std::{collections::BTreeMap, fmt, io::Write, path::Path};

use base64::{Engine as _, engine::general_purpose::STANDARD};
use image::ImageFormat;
use infer_runtime_client::{
    BoundingBox, Client as SdkClient, DiscoveryResolver, FaceDetectionResponse,
    FaceEmbeddingResponse, FaceParsingResponse, FivePointLandmarks, Point,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest as _, Sha256};
use tempfile::NamedTempFile;
use tokio::runtime::{Builder as RuntimeBuilder, Runtime};

use crate::{FaceBoundingBox, FaceEmbedding, FaceLandmarks, FacePoint};

pub use image_completion::{INFER_IMAGE_COMPLETION_CAPABILITY, InferImageCompletionEvidence};
pub use image_understanding::{
    ClassificationReviewCategory, ClassificationReviewDisposition, ClassificationReviewEvidence,
    ClassificationReviewProvider, ClassificationReviewRequest, ClassificationReviewSuggestion,
    ImageUnderstandingEvidence, ImageUnderstandingProvenance, ImageUnderstandingProvider,
    ImageUnderstandingQuality,
};
pub use infer_runtime_client::{
    AttemptSnapshot as InferRuntimeAttemptSnapshot, CancelResult as InferRuntimeCancelResult,
    CapabilityCatalog as InferRuntimeCapabilityCatalog, ContractManifest as InferRuntimeContract,
    Error as InferRuntimeClientError, ExplainResult as InferRuntimeExplainResult,
    JobListPage as InferRuntimeJobListPage, JobSnapshot as InferRuntimeJobSnapshot,
};
pub use raw_foundation::{
    InferRawFoundationArtifactReceipt, InferRawFoundationCancellation,
    InferRawFoundationDecoderIdentity, InferRawFoundationJob, InferRawFoundationLeaseGrant,
    InferRawFoundationPriority, InferRawFoundationProvenance, InferRawFoundationProvider,
    InferRawFoundationRegisteredLease, InferRawFoundationRequest, InferRawFoundationResult,
    InferRawFoundationSource, InferRawFoundationStaging, RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256,
    RAWNIND_FOUNDATION_MODEL_ID, RAWNIND_FOUNDATION_PACKAGE_SHA256,
    RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256, infer_raw_foundation_sdk_status,
};
pub use semantic::{
    ImageEmbeddingEvidence, SemanticEmbeddingProvider, SemanticRequestPriority,
    TextEmbeddingEvidence,
};
pub use semantic_grounding::{
    INFER_SEMANTIC_GROUNDING_CAPABILITY, SemanticGroundedRegion, SemanticGroundingEvidence,
};
pub use subject_mask::{INFER_SUBJECT_MASK_CAPABILITY, InferSubjectMaskEvidence};

const MAX_DETECTIONS: usize = 4_096;
const EXPECTED_FACE_ORIENTATION: &str = "input_pixels_no_exif_transform";
const EXPECTED_FACE_PARSING_ORIENTATION: &str = "display_pixels_orientation_normalized";
const BIOMETRIC_CLASSIFICATION: &str = "sensitive_biometric";
pub const FACE_PARSING_ONTOLOGY: &str = "celebamask_hq_19";
const FACE_PARSING_CLASS_COUNT: usize = 19;
const FACE_PARSING_CLASS_IDS: [&str; FACE_PARSING_CLASS_COUNT] = [
    "background",
    "skin",
    "left_eyebrow",
    "right_eyebrow",
    "left_eye",
    "right_eye",
    "eyeglasses",
    "left_ear",
    "right_ear",
    "earring",
    "nose",
    "mouth",
    "upper_lip",
    "lower_lip",
    "neck",
    "necklace",
    "clothing",
    "hair",
    "hat",
];

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct VisionProvenance {
    pub job_id: String,
    pub provider: String,
    pub deployment: String,
    pub model_build: String,
    pub artifact_sha256: String,
    pub preprocessing_identity: String,
    pub postprocessing_identity: String,
    pub tokenizer: Option<VisionTokenizerProvenance>,
    pub runtime: String,
    pub requested_execution_provider: String,
    pub actual_execution_provider: String,
    pub execution_provider_fallback_reason: Option<String>,
    pub precision: String,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct VisionTokenizerProvenance {
    pub identity: String,
    pub artifact_sha256: String,
    pub max_length: usize,
    pub lowercase: bool,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct DetectedFace {
    pub bounding_box: FaceBoundingBox,
    pub landmarks: FaceLandmarks,
    pub confidence: f32,
}

#[derive(Debug, Clone, PartialEq)]
pub struct DetectedFaceBatch {
    pub source_revision: String,
    pub width: u32,
    pub height: u32,
    pub orientation: String,
    pub detections: Vec<DetectedFace>,
    pub provenance: VisionProvenance,
}

#[derive(Debug, Copy, Clone, PartialEq, Deserialize)]
pub struct FaceEmbeddingEligibility {
    pub eligible: bool,
    pub landmarks_in_image: bool,
    pub inter_eye_distance_pixels: f32,
    pub alignment_rmse_pixels: f32,
}

#[derive(Debug, Clone, PartialEq)]
pub struct EmbeddedFace {
    pub source_revision: String,
    pub embedding: FaceEmbedding,
    pub eligibility: FaceEmbeddingEligibility,
    pub provenance: VisionProvenance,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ParsedFace {
    pub source_revision: String,
    pub width: u32,
    pub height: u32,
    pub labels: Vec<u8>,
    pub provenance: VisionProvenance,
}

pub trait FaceAnalysisProvider {
    /// # Errors
    ///
    /// Returns a provider-specific error without logging image bytes.
    fn detect_faces(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
    ) -> Result<DetectedFaceBatch, InferRuntimeClientError>;

    /// # Errors
    ///
    /// Returns a provider-specific error without logging image bytes or the
    /// returned biometric vector.
    fn embed_face(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        landmarks: FaceLandmarks,
    ) -> Result<EmbeddedFace, InferRuntimeClientError>;

    /// Runs detection with a cooperative cancellation boundary.
    /// # Errors
    /// Returns the underlying provider error.
    fn detect_faces_cancellable(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<DetectedFaceBatch>, InferRuntimeClientError> {
        if cancelled() {
            return Ok(None);
        }
        let result = self.detect_faces(image, media_type, source_revision)?;
        Ok((!cancelled()).then_some(result))
    }

    /// Runs embedding with a cooperative cancellation boundary.
    /// # Errors
    /// Returns the underlying provider error.
    fn embed_face_cancellable(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        landmarks: FaceLandmarks,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<EmbeddedFace>, InferRuntimeClientError> {
        if cancelled() {
            return Ok(None);
        }
        let result = self.embed_face(image, media_type, source_revision, landmarks)?;
        Ok((!cancelled()).then_some(result))
    }

    /// Returns one full-image CelebAMask-HQ label map for the selected face.
    ///
    /// # Errors
    ///
    /// Returns a provider-specific error without logging image bytes.
    fn parse_face(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        face_box: FaceBoundingBox,
    ) -> Result<ParsedFace, InferRuntimeClientError>;
}

/// Synchronous product adapter over the official asynchronous SDK.
pub struct InferRuntimeClient {
    runtime: Runtime,
    sdk: SdkClient,
}

impl fmt::Debug for InferRuntimeClient {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InferRuntimeClient")
            .field("sdk", &"infer-runtime-client@1.0.0")
            .finish_non_exhaustive()
    }
}

impl InferRuntimeClient {
    /// Uses an explicit, path-free loopback endpoint for development or
    /// diagnostics. This is an override, never a product fallback.
    ///
    /// # Errors
    ///
    /// Returns SDK endpoint, client-construction, or executor failures.
    pub fn from_credential_file(
        base_url: &str,
        credential_path: &Path,
    ) -> Result<Self, InferRuntimeClientError> {
        Self::from_credential_file_with_discovery(Some(base_url), credential_path)
    }

    /// Uses official Infra Discovery unless an explicit development endpoint
    /// is supplied. No fixed-port or candidate-contract fallback exists.
    ///
    /// # Errors
    ///
    /// Returns SDK endpoint, client-construction, or executor failures.
    pub fn from_credential_file_with_discovery(
        explicit_base_url: Option<&str>,
        credential_path: &Path,
    ) -> Result<Self, InferRuntimeClientError> {
        let mut resolver = DiscoveryResolver::local();
        if let Some(endpoint) = explicit_base_url.filter(|value| !value.is_empty()) {
            resolver = resolver.with_explicit_endpoint(endpoint.to_owned())?;
        }
        let sdk = SdkClient::with_discovery(resolver)
            .credential_file(credential_path)
            .build()?;
        let runtime = RuntimeBuilder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|error| {
                InferRuntimeClientError::Input(format!(
                    "cannot construct Shadow's Infer Runtime SDK executor: {error}"
                ))
            })?;
        Ok(Self { runtime, sdk })
    }

    /// Returns the exact dated Core contract after SDK schema verification.
    ///
    /// # Errors
    ///
    /// Returns SDK Discovery, transport, schema, or response failures.
    pub fn contract(&self) -> Result<InferRuntimeContract, InferRuntimeClientError> {
        self.runtime.block_on(self.sdk.contract())
    }

    /// Returns the SDK-validated dated Capability Catalog.
    ///
    /// # Errors
    ///
    /// Returns SDK Discovery, transport, schema, or response failures.
    pub fn capabilities(&self) -> Result<InferRuntimeCapabilityCatalog, InferRuntimeClientError> {
        self.runtime.block_on(self.sdk.capabilities())
    }

    /// Queries one payload-free Job projection through the official Core API.
    ///
    /// # Errors
    ///
    /// Returns SDK Job-id, Discovery, transport, or response failures.
    pub fn job(&self, job_id: &str) -> Result<InferRuntimeJobSnapshot, InferRuntimeClientError> {
        self.runtime.block_on(self.sdk.job(job_id))
    }

    /// Queries a bounded payload-free Job page through the official Core API.
    ///
    /// # Errors
    ///
    /// Returns SDK Discovery, transport, or response failures.
    pub fn jobs(
        &self,
        query: &[(&str, &str)],
    ) -> Result<InferRuntimeJobListPage, InferRuntimeClientError> {
        self.runtime.block_on(self.sdk.jobs(query))
    }

    /// Queries routing and Attempt provenance through the official Core API.
    ///
    /// # Errors
    ///
    /// Returns SDK Job-id, Discovery, transport, or response failures.
    pub fn explain(
        &self,
        job_id: &str,
    ) -> Result<InferRuntimeExplainResult, InferRuntimeClientError> {
        self.runtime.block_on(self.sdk.explain(job_id))
    }

    /// Cancels a Job through the official Core API.
    ///
    /// # Errors
    ///
    /// Returns SDK Job-id, Discovery, transport, or response failures.
    pub fn cancel_job(
        &self,
        job_id: &str,
    ) -> Result<InferRuntimeCancelResult, InferRuntimeClientError> {
        self.runtime.block_on(self.sdk.cancel_job(job_id))
    }

    fn stage_image(
        image: &[u8],
        media_type: &str,
        revision: &str,
    ) -> Result<(NamedTempFile, &'static str), InferRuntimeClientError> {
        if image.is_empty() || image.len() > 20 * 1024 * 1024 {
            return Err(InferRuntimeClientError::Input(
                "image source violates the 20 MiB typed vision bound".into(),
            ));
        }
        let media_type = match media_type {
            "image/jpeg" => "image/jpeg",
            "image/png" => "image/png",
            _ => {
                return Err(InferRuntimeClientError::Input(
                    "typed vision accepts only image/jpeg or image/png".into(),
                ));
            }
        };
        if revision.trim().is_empty() || revision.len() > 256 {
            return Err(InferRuntimeClientError::Input(
                "typed vision revision must be non-empty and at most 256 bytes".into(),
            ));
        }
        let mut staged = NamedTempFile::new().map_err(|error| {
            InferRuntimeClientError::Input(format!("cannot stage bounded vision input: {error}"))
        })?;
        staged.write_all(image).map_err(|error| {
            InferRuntimeClientError::Input(format!("cannot stage bounded vision input: {error}"))
        })?;
        staged.flush().map_err(|error| {
            InferRuntimeClientError::Input(format!("cannot flush bounded vision input: {error}"))
        })?;
        Ok((staged, media_type))
    }

    fn sdk(&self) -> &SdkClient {
        &self.sdk
    }

    fn block_on_cancellable<T>(
        &self,
        future: impl std::future::Future<Output = Result<T, InferRuntimeClientError>>,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<T>, InferRuntimeClientError> {
        if cancelled() {
            return Ok(None);
        }
        self.runtime.block_on(async {
            tokio::select! {
                biased;
                () = async {
                    while !cancelled() {
                        tokio::time::sleep(std::time::Duration::from_millis(10)).await;
                    }
                } => Ok(None),
                result = future => result.map(Some),
            }
        })
    }

    fn block_on<T>(
        &self,
        future: impl std::future::Future<Output = Result<T, InferRuntimeClientError>>,
    ) -> Result<T, InferRuntimeClientError> {
        self.runtime.block_on(future)
    }
}

impl FaceAnalysisProvider for InferRuntimeClient {
    fn detect_faces(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
    ) -> Result<DetectedFaceBatch, InferRuntimeClientError> {
        self.detect_faces_cancellable(image, media_type, source_revision, &|| false)?
            .ok_or_else(|| InferRuntimeClientError::Input("face detection cancelled".into()))
    }
    fn embed_face(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        landmarks: FaceLandmarks,
    ) -> Result<EmbeddedFace, InferRuntimeClientError> {
        self.embed_face_cancellable(image, media_type, source_revision, landmarks, &|| false)?
            .ok_or_else(|| InferRuntimeClientError::Input("face embedding cancelled".into()))
    }

    fn detect_faces_cancellable(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<DetectedFaceBatch>, InferRuntimeClientError> {
        let (staged, media_type) = Self::stage_image(image, media_type, source_revision)?;
        let metadata = local_metadata("background", None);
        let response = self.block_on_cancellable(
            self.sdk
                .detect_faces(staged.path(), media_type, source_revision, &metadata),
            cancelled,
        )?;
        response
            .map(|value| admit_face_detection(value, source_revision))
            .transpose()
    }

    fn embed_face_cancellable(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        landmarks: FaceLandmarks,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<EmbeddedFace>, InferRuntimeClientError> {
        if landmarks
            .points()
            .iter()
            .any(|point| !point.x.is_finite() || !point.y.is_finite())
        {
            return malformed("face landmarks must be finite");
        }
        let (staged, media_type) = Self::stage_image(image, media_type, source_revision)?;
        let metadata = local_metadata("background", None);
        let response = self.block_on_cancellable(
            self.sdk.embed_face(
                staged.path(),
                media_type,
                source_revision,
                sdk_landmarks(landmarks),
                &metadata,
            ),
            cancelled,
        )?;
        response
            .map(|value| admit_face_embedding(value, source_revision))
            .transpose()
    }

    fn parse_face(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        face_box: FaceBoundingBox,
    ) -> Result<ParsedFace, InferRuntimeClientError> {
        if !valid_face_box(face_box) {
            return malformed("face parsing requires a finite positive face box");
        }
        let (staged, media_type) = Self::stage_image(image, media_type, source_revision)?;
        let metadata = local_metadata("interactive", None);
        let response = self.block_on(self.sdk.parse_face(
            staged.path(),
            media_type,
            source_revision,
            BoundingBox {
                x: face_box.x,
                y: face_box.y,
                width: face_box.width,
                height: face_box.height,
            },
            &metadata,
        ))?;
        admit_face_parsing(response, source_revision, face_box)
    }
}

fn admit_face_detection(
    response: FaceDetectionResponse,
    expected_source_revision: &str,
) -> Result<DetectedFaceBatch, InferRuntimeClientError> {
    if response.object != "vision.face_detection"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.image.orientation != EXPECTED_FACE_ORIENTATION
        || response.image.width == 0
        || response.image.height == 0
        || response.detections.len() > MAX_DETECTIONS
    {
        return malformed("face detection response violated Shadow's typed evidence contract");
    }
    let detections = response
        .detections
        .into_iter()
        .map(|detection| DetectedFace {
            bounding_box: FaceBoundingBox {
                x: detection.bounding_box.x,
                y: detection.bounding_box.y,
                width: detection.bounding_box.width,
                height: detection.bounding_box.height,
            },
            landmarks: shadow_landmarks(detection.landmarks),
            confidence: detection.confidence,
        })
        .collect::<Vec<_>>();
    if detections
        .iter()
        .any(|detection| !valid_detection(detection, response.image.width, response.image.height))
    {
        return malformed("face detection geometry violated Shadow's evidence contract");
    }
    Ok(DetectedFaceBatch {
        source_revision: response.source_revision,
        width: response.image.width,
        height: response.image.height,
        orientation: response.image.orientation,
        detections,
        provenance: admit_vision_provenance(response.provenance)?,
    })
}

fn admit_face_embedding(
    response: FaceEmbeddingResponse,
    expected_source_revision: &str,
) -> Result<EmbeddedFace, InferRuntimeClientError> {
    if response.object != "vision.face_embedding"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.data_classification != BIOMETRIC_CLASSIFICATION
        || response.embedding.dimensions != crate::SFACE_EMBEDDING_DIMENSIONS
        || response.embedding.values.len() != response.embedding.dimensions
        || !response.embedding.normalized
        || response.embedding.distance_metric != "cosine"
        || !response.eligibility.eligible
        || !response.eligibility.landmarks_in_image
        || !response.eligibility.inter_eye_distance_pixels.is_finite()
        || response.eligibility.inter_eye_distance_pixels < 0.0
        || !response.eligibility.alignment_rmse_pixels.is_finite()
        || response.eligibility.alignment_rmse_pixels < 0.0
    {
        return malformed("face embedding response violated Shadow's biometric evidence contract");
    }
    let embedding = FaceEmbedding::new(response.embedding.values, response.embedding.space)
        .map_err(|_| {
            InferRuntimeClientError::MalformedResponse(
                "face embedding values violated Shadow's SFace contract".into(),
            )
        })?;
    Ok(EmbeddedFace {
        source_revision: response.source_revision,
        embedding,
        eligibility: FaceEmbeddingEligibility {
            eligible: response.eligibility.eligible,
            landmarks_in_image: response.eligibility.landmarks_in_image,
            inter_eye_distance_pixels: response.eligibility.inter_eye_distance_pixels,
            alignment_rmse_pixels: response.eligibility.alignment_rmse_pixels,
        },
        provenance: admit_vision_provenance(response.provenance)?,
    })
}

fn admit_face_parsing(
    response: FaceParsingResponse,
    expected_source_revision: &str,
    expected_face_box: FaceBoundingBox,
) -> Result<ParsedFace, InferRuntimeClientError> {
    if response.object != "vision.face_parsing"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.data_classification != BIOMETRIC_CLASSIFICATION
        || response.image.orientation != EXPECTED_FACE_PARSING_ORIENTATION
        || response.image.width == 0
        || response.image.height == 0
        || response.label_map.content_type != "image/png"
        || response.label_map.encoding != "indexed_u8_png"
        || response.label_map.width != response.image.width
        || response.label_map.height != response.image.height
        || response.ontology.id != FACE_PARSING_ONTOLOGY
        || response.ontology.background_value != 0
        || response.ontology.class_count != FACE_PARSING_CLASS_COUNT
        || response.regions.len() != FACE_PARSING_CLASS_COUNT
        || !same_face_box(&response.face_box, expected_face_box)
    {
        return malformed("face parsing response violated Shadow's typed evidence contract");
    }
    let encoded = STANDARD
        .decode(&response.label_map.data_base64)
        .map_err(|_| {
            InferRuntimeClientError::MalformedResponse(
                "face parsing label map is not valid base64".into(),
            )
        })?;
    if format!("{:x}", Sha256::digest(&encoded)) != response.label_map.sha256 {
        return malformed("face parsing label-map digest does not match its evidence");
    }
    let decoded =
        image::load_from_memory_with_format(&encoded, ImageFormat::Png).map_err(|_| {
            InferRuntimeClientError::MalformedResponse(
                "face parsing label map is not a decodable PNG".into(),
            )
        })?;
    if decoded.color() != image::ColorType::L8 {
        return malformed("face parsing label map is not an indexed Gray8 raster");
    }
    let decoded = decoded.into_luma8();
    if decoded.width() != response.image.width || decoded.height() != response.image.height {
        return malformed("face parsing label map dimensions changed after decoding");
    }
    let mut class_counts = [0_u64; FACE_PARSING_CLASS_COUNT];
    for label in decoded.as_raw() {
        let Some(count) = class_counts.get_mut(usize::from(*label)) else {
            return malformed("face parsing label map contains an unknown class");
        };
        *count += 1;
    }
    if response.regions.iter().enumerate().any(|(index, region)| {
        usize::from(region.label_value) != index
            || region.class_id
                != format!(
                    "{}:{}",
                    FACE_PARSING_ONTOLOGY, FACE_PARSING_CLASS_IDS[index]
                )
            || region.pixel_count != class_counts[index]
    }) {
        return malformed("face parsing label map violated the CelebAMask-HQ ontology");
    }
    Ok(ParsedFace {
        source_revision: response.source_revision,
        width: response.image.width,
        height: response.image.height,
        labels: decoded.into_raw(),
        provenance: admit_vision_provenance(response.provenance)?,
    })
}

fn same_face_box(actual: &BoundingBox, expected: FaceBoundingBox) -> bool {
    (actual.x - expected.x).abs() <= 0.01
        && (actual.y - expected.y).abs() <= 0.01
        && (actual.width - expected.width).abs() <= 0.01
        && (actual.height - expected.height).abs() <= 0.01
}

fn admit_vision_provenance(
    mut provenance: infer_runtime_client::VisionProvenance,
) -> Result<VisionProvenance, InferRuntimeClientError> {
    let tokenizer = provenance
        .extra
        .remove("tokenizer")
        .map(serde_json::from_value::<VisionTokenizerProvenance>)
        .transpose()
        .map_err(|error| {
            InferRuntimeClientError::MalformedResponse(format!(
                "typed vision tokenizer provenance is malformed: {error}"
            ))
        })?;
    let admitted = VisionProvenance {
        job_id: provenance.job_id,
        provider: provenance.provider,
        deployment: provenance.deployment,
        model_build: provenance.model_build,
        artifact_sha256: provenance.artifact_sha256,
        preprocessing_identity: provenance.preprocessing_identity,
        postprocessing_identity: provenance.postprocessing_identity,
        tokenizer,
        runtime: provenance.runtime,
        requested_execution_provider: provenance.requested_execution_provider,
        actual_execution_provider: provenance.actual_execution_provider,
        execution_provider_fallback_reason: provenance.execution_provider_fallback_reason,
        precision: provenance.precision,
    };
    if !valid_provenance(&admitted) {
        return malformed("typed vision provenance violated Shadow's evidence contract");
    }
    Ok(admitted)
}

fn local_metadata(priority: &str, capability_floor: Option<&str>) -> BTreeMap<String, String> {
    let mut metadata = BTreeMap::from([
        ("infer.priority".into(), priority.into()),
        ("infer.placement".into(), "local_only".into()),
        ("infer.offline_required".into(), "true".into()),
        ("infer.fallback".into(), "none".into()),
    ]);
    if let Some(floor) = capability_floor {
        metadata.insert("infer.capability_floor".into(), floor.into());
    }
    metadata
}

fn sdk_landmarks(value: FaceLandmarks) -> FivePointLandmarks {
    FivePointLandmarks {
        right_eye: sdk_point(value.right_eye),
        left_eye: sdk_point(value.left_eye),
        nose_tip: sdk_point(value.nose_tip),
        right_mouth_corner: sdk_point(value.right_mouth_corner),
        left_mouth_corner: sdk_point(value.left_mouth_corner),
    }
}

fn sdk_point(value: FacePoint) -> Point {
    Point {
        x: value.x,
        y: value.y,
    }
}

fn shadow_landmarks(value: FivePointLandmarks) -> FaceLandmarks {
    FaceLandmarks {
        right_eye: shadow_point(value.right_eye),
        left_eye: shadow_point(value.left_eye),
        nose_tip: shadow_point(value.nose_tip),
        right_mouth_corner: shadow_point(value.right_mouth_corner),
        left_mouth_corner: shadow_point(value.left_mouth_corner),
    }
}

fn shadow_point(value: Point) -> FacePoint {
    FacePoint {
        x: value.x,
        y: value.y,
    }
}

fn valid_detection(detection: &DetectedFace, width: u32, height: u32) -> bool {
    let width = f64::from(width);
    let height = f64::from(height);
    let bounding_box = detection.bounding_box;
    let values = [
        bounding_box.x,
        bounding_box.y,
        bounding_box.width,
        bounding_box.height,
        detection.confidence,
    ];
    values.iter().all(|value| value.is_finite())
        && bounding_box.x >= 0.0
        && bounding_box.y >= 0.0
        && bounding_box.width > 0.0
        && bounding_box.height > 0.0
        && f64::from(bounding_box.x + bounding_box.width) <= width + 0.01
        && f64::from(bounding_box.y + bounding_box.height) <= height + 0.01
        && (0.0..=1.0).contains(&detection.confidence)
        && detection.landmarks.points().iter().all(|point| {
            point.x.is_finite()
                && point.y.is_finite()
                && point.x >= 0.0
                && point.y >= 0.0
                // YuNet clips detection landmarks to the closed image extent.
                // SFace separately checks strict interior/alignment eligibility;
                // one clipped face must not invalidate the whole detection batch.
                && f64::from(point.x) <= width
                && f64::from(point.y) <= height
        })
}

fn valid_face_box(value: FaceBoundingBox) -> bool {
    [value.x, value.y, value.width, value.height]
        .iter()
        .all(|component| component.is_finite())
        && value.x >= 0.0
        && value.y >= 0.0
        && value.width > 0.0
        && value.height > 0.0
}

fn valid_provenance(provenance: &VisionProvenance) -> bool {
    let core_is_valid = [
        provenance.job_id.as_str(),
        provenance.provider.as_str(),
        provenance.deployment.as_str(),
        provenance.model_build.as_str(),
        provenance.artifact_sha256.as_str(),
        provenance.preprocessing_identity.as_str(),
        provenance.postprocessing_identity.as_str(),
        provenance.runtime.as_str(),
        provenance.requested_execution_provider.as_str(),
        provenance.actual_execution_provider.as_str(),
        provenance.precision.as_str(),
    ]
    .iter()
    .all(|value| !value.trim().is_empty() && value.len() <= 1_024);
    let tokenizer_is_valid = provenance.tokenizer.as_ref().is_none_or(|tokenizer| {
        !tokenizer.identity.trim().is_empty()
            && tokenizer.identity.len() <= 1_024
            && !tokenizer.artifact_sha256.trim().is_empty()
            && tokenizer.artifact_sha256.len() <= 1_024
            && tokenizer.max_length > 0
            && tokenizer.max_length <= 4_096
    });
    core_is_valid && tokenizer_is_valid
}

fn malformed<T>(message: &str) -> Result<T, InferRuntimeClientError> {
    Err(InferRuntimeClientError::MalformedResponse(message.into()))
}

#[cfg(test)]
mod tests;

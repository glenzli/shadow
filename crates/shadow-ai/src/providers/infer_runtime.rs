//! Fail-closed loopback client for infer-runtime's experimental typed face APIs.
//!
//! The bearer credential is always redacted, redirects are disabled, and the
//! base URL is restricted to the local machine so biometric image bytes cannot
//! be redirected to a remote service.

mod discovery;
mod image_understanding;
mod semantic;

use std::{
    fmt, fs,
    io::{self, Read},
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};

use reqwest::{
    Url,
    blocking::{Client, RequestBuilder, multipart},
    redirect::Policy,
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use thiserror::Error;

use crate::{FaceBoundingBox, FaceEmbedding, FaceLandmarks};
use discovery::{DiscoveryEndpoint, InferRuntimeConsumerVersion, InferRuntimeDiscoveryResolver};

pub use image_understanding::{
    ClassificationReviewCategory, ClassificationReviewDisposition, ClassificationReviewEvidence,
    ClassificationReviewProvider, ClassificationReviewRequest, ClassificationReviewSuggestion,
    ImageUnderstandingEvidence, ImageUnderstandingProvenance, ImageUnderstandingProvider,
    ImageUnderstandingQuality,
};
pub use semantic::{
    ImageEmbeddingEvidence, SemanticEmbeddingProvider, SemanticRequestPriority,
    TextEmbeddingEvidence,
};

const DETECT_FACES_PATH: &str = "infer/v1/vision/face-detections";
const EMBED_FACE_PATH: &str = "infer/v1/vision/face-embeddings";
const MAX_IMAGE_BYTES: usize = 20 * 1024 * 1024;
const MAX_RESPONSE_BYTES: usize = 4 * 1024 * 1024;
const MAX_DETECTIONS: usize = 4_096;
const EXPECTED_ORIENTATION: &str = "input_pixels_no_exif_transform";
const BIOMETRIC_CLASSIFICATION: &str = "sensitive_biometric";

#[derive(Clone)]
pub struct InferRuntimeCredential(Arc<str>);

impl fmt::Debug for InferRuntimeCredential {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("InferRuntimeCredential(<redacted>)")
    }
}

impl InferRuntimeCredential {
    /// Loads one owner-only token without exposing it through command-line
    /// arguments, debug formatting, or error text.
    ///
    /// # Errors
    ///
    /// Rejects symlinks, non-files, group/world access on Unix, malformed
    /// token text, or filesystem failures.
    pub fn load(path: &Path) -> Result<Self, InferRuntimeClientError> {
        let metadata =
            fs::symlink_metadata(path).map_err(|source| InferRuntimeClientError::CredentialIo {
                path: path.to_path_buf(),
                source,
            })?;
        if metadata.file_type().is_symlink() || !metadata.file_type().is_file() {
            return Err(InferRuntimeClientError::UnsafeCredentialFile(
                path.to_path_buf(),
            ));
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if metadata.permissions().mode() & 0o077 != 0 {
                return Err(InferRuntimeClientError::UnsafeCredentialPermissions(
                    path.to_path_buf(),
                ));
            }
        }
        let value =
            fs::read_to_string(path).map_err(|source| InferRuntimeClientError::CredentialIo {
                path: path.to_path_buf(),
                source,
            })?;
        Self::parse(value.trim_end_matches(['\r', '\n']))
    }

    /// Creates a credential from an already protected secret source.
    ///
    /// # Errors
    ///
    /// Rejects short, oversized, whitespace-containing, or control-containing
    /// values.
    pub fn parse(value: &str) -> Result<Self, InferRuntimeClientError> {
        if !(32..=512).contains(&value.len())
            || value
                .bytes()
                .any(|byte| byte.is_ascii_control() || byte.is_ascii_whitespace())
        {
            return Err(InferRuntimeClientError::InvalidCredential);
        }
        Ok(Self(Arc::from(value)))
    }

    fn expose(&self) -> &str {
        &self.0
    }
}

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
}

#[derive(Debug, Clone)]
pub struct InferRuntimeClient {
    client: Client,
    endpoint: InferRuntimeEndpoint,
    credential: InferRuntimeCredential,
}

#[derive(Debug, Clone)]
enum InferRuntimeEndpoint {
    Fixed(Url),
    Discovery(Arc<InferRuntimeDiscoveryResolver>),
}

impl InferRuntimeClient {
    /// Creates a local-only client. HTTP redirects are disabled.
    ///
    /// # Errors
    ///
    /// Rejects non-loopback, non-HTTP, credential-bearing, or path-bearing
    /// base URLs and client construction failures.
    pub fn new(
        base_url: &str,
        credential: InferRuntimeCredential,
    ) -> Result<Self, InferRuntimeClientError> {
        let base_url = validate_loopback_base_url(base_url)?;
        Self::with_endpoint(InferRuntimeEndpoint::Fixed(base_url), credential)
    }

    /// Creates a local-only client using the Consumer endpoint selection
    /// order: explicit override, Infra Discovery, then the temporary fixed
    /// loopback fallback.
    ///
    /// # Errors
    ///
    /// Rejects an invalid explicit override or HTTP client construction
    /// failure. Discovery failures remain contained and select the migration
    /// fallback instead of changing credential or typed-request behavior.
    pub fn discover(
        explicit_base_url: Option<&str>,
        credential: InferRuntimeCredential,
    ) -> Result<Self, InferRuntimeClientError> {
        if let Some(base_url) = explicit_base_url.filter(|value| !value.is_empty()) {
            return Self::new(base_url, credential);
        }
        Self::with_endpoint(
            InferRuntimeEndpoint::Discovery(Arc::new(
                InferRuntimeDiscoveryResolver::from_environment(),
            )),
            credential,
        )
    }

    fn with_endpoint(
        endpoint: InferRuntimeEndpoint,
        credential: InferRuntimeCredential,
    ) -> Result<Self, InferRuntimeClientError> {
        let client = Client::builder()
            .connect_timeout(Duration::from_secs(3))
            .timeout(Duration::from_mins(1))
            .no_proxy()
            .redirect(Policy::none())
            .build()
            .map_err(InferRuntimeClientError::ClientBuild)?;
        Ok(Self {
            client,
            endpoint,
            credential,
        })
    }

    /// Loads an owner-only credential file and creates a local-only client.
    ///
    /// # Errors
    ///
    /// Returns credential or client configuration failures.
    pub fn from_credential_file(
        base_url: &str,
        credential_path: &Path,
    ) -> Result<Self, InferRuntimeClientError> {
        Self::new(base_url, InferRuntimeCredential::load(credential_path)?)
    }

    /// Loads the existing owner-only credential and resolves the endpoint via
    /// explicit override, Infra Discovery, then the migration fallback.
    ///
    /// # Errors
    ///
    /// Returns credential, explicit endpoint, or client construction failures.
    pub fn from_credential_file_with_discovery(
        explicit_base_url: Option<&str>,
        credential_path: &Path,
    ) -> Result<Self, InferRuntimeClientError> {
        Self::discover(
            explicit_base_url,
            InferRuntimeCredential::load(credential_path)?,
        )
    }

    fn resolve_endpoint(&self) -> DiscoveryEndpoint {
        match &self.endpoint {
            InferRuntimeEndpoint::Fixed(base_url) => DiscoveryEndpoint::explicit(base_url.clone()),
            InferRuntimeEndpoint::Discovery(resolver) => resolver.resolve(),
        }
    }

    fn endpoint_url(
        endpoint: &DiscoveryEndpoint,
        path: &str,
    ) -> Result<Url, InferRuntimeClientError> {
        endpoint
            .base_url
            .join(path)
            .map_err(|_| InferRuntimeClientError::InvalidBaseUrl)
    }
}

impl FaceAnalysisProvider for InferRuntimeClient {
    fn detect_faces(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
    ) -> Result<DetectedFaceBatch, InferRuntimeClientError> {
        validate_request(image, media_type, source_revision)?;
        let response: RawFaceDetectionResponse =
            self.send_json(DETECT_FACES_PATH, |endpoint, _consumer_version| {
                let form = image_form("vision.detect_faces", image, media_type, source_revision)?;
                Ok(self
                    .client
                    .post(endpoint)
                    .bearer_auth(self.credential.expose())
                    .multipart(form))
            })?;
        response.validate(source_revision)
    }

    fn embed_face(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        landmarks: FaceLandmarks,
    ) -> Result<EmbeddedFace, InferRuntimeClientError> {
        validate_request(image, media_type, source_revision)?;
        if landmarks
            .points()
            .iter()
            .any(|point| !point.x.is_finite() || !point.y.is_finite())
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "face landmarks must be finite",
            ));
        }
        let landmarks_json =
            serde_json::to_string(&landmarks).map_err(InferRuntimeClientError::SerializeRequest)?;
        let response: RawFaceEmbeddingResponse =
            self.send_json(EMBED_FACE_PATH, |endpoint, _consumer_version| {
                let form = image_form("vision.embed_face", image, media_type, source_revision)?
                    .text("landmarks", landmarks_json.clone());
                Ok(self
                    .client
                    .post(endpoint)
                    .bearer_auth(self.credential.expose())
                    .multipart(form))
            })?;
        response.validate(source_revision)
    }
}

impl InferRuntimeClient {
    fn send_json<T: DeserializeOwned>(
        &self,
        path: &str,
        mut build_request: impl FnMut(
            Url,
            InferRuntimeConsumerVersion,
        ) -> Result<RequestBuilder, InferRuntimeClientError>,
    ) -> Result<T, InferRuntimeClientError> {
        let first_endpoint = self.resolve_endpoint();
        let request = build_request(
            Self::endpoint_url(&first_endpoint, path)?,
            first_endpoint.consumer_version,
        )?;
        let first_result = Self::send_json_once(request);
        let should_rediscover = matches!(
            &first_result,
            Err(InferRuntimeClientError::Request(error)) if error.is_connect()
        );
        if !should_rediscover {
            return first_result;
        }
        let InferRuntimeEndpoint::Discovery(resolver) = &self.endpoint else {
            return first_result;
        };
        let retry_endpoint = resolver.resolve_after_connection_failure(&first_endpoint);
        if retry_endpoint == first_endpoint {
            return first_result;
        }
        let retry = build_request(
            Self::endpoint_url(&retry_endpoint, path)?,
            retry_endpoint.consumer_version,
        )?;
        Self::send_json_once(retry)
    }

    fn send_json_once<T: DeserializeOwned>(
        request: RequestBuilder,
    ) -> Result<T, InferRuntimeClientError> {
        let mut response = request.send().map_err(InferRuntimeClientError::Request)?;
        if response
            .content_length()
            .is_some_and(|length| length > MAX_RESPONSE_BYTES as u64)
        {
            return Err(InferRuntimeClientError::ResponseTooLarge);
        }
        let status = response.status();
        let bytes = read_bounded(&mut response)?;
        if !status.is_success() {
            let code = serde_json::from_slice::<ErrorEnvelope>(&bytes).map_or_else(
                |_| "invalid_error_response".into(),
                |envelope| {
                    let code = envelope.error.code;
                    if !code.is_empty()
                        && code.len() <= 128
                        && code.bytes().all(|byte| {
                            byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_')
                        })
                    {
                        code
                    } else {
                        "invalid_error_response".into()
                    }
                },
            );
            return Err(InferRuntimeClientError::Http {
                status: status.as_u16(),
                code,
            });
        }
        serde_json::from_slice(&bytes).map_err(InferRuntimeClientError::DecodeResponse)
    }
}

fn validate_loopback_base_url(value: &str) -> Result<Url, InferRuntimeClientError> {
    let invalid = || InferRuntimeClientError::InvalidBaseUrl;
    let address = value
        .strip_prefix("http://")
        .ok_or_else(invalid)?
        .parse::<std::net::SocketAddr>()
        .map_err(|_| invalid())?;
    if !address.ip().is_loopback() || address.port() == 0 || format!("http://{address}") != value {
        return Err(InferRuntimeClientError::InvalidBaseUrl);
    }
    let mut url = Url::parse(value).map_err(|_| InferRuntimeClientError::InvalidBaseUrl)?;
    url.set_path("/");
    Ok(url)
}

fn validate_request(
    image: &[u8],
    media_type: &str,
    source_revision: &str,
) -> Result<(), InferRuntimeClientError> {
    if image.is_empty() || image.len() > MAX_IMAGE_BYTES {
        return Err(InferRuntimeClientError::InvalidImageLength(image.len()));
    }
    if !matches!(media_type, "image/jpeg" | "image/png") {
        return Err(InferRuntimeClientError::InvalidMediaType);
    }
    if source_revision.is_empty() || source_revision.len() > 256 {
        return Err(InferRuntimeClientError::InvalidSourceRevision);
    }
    Ok(())
}

fn image_form(
    model: &'static str,
    image: &[u8],
    media_type: &str,
    source_revision: &str,
) -> Result<multipart::Form, InferRuntimeClientError> {
    let extension = if media_type == "image/png" {
        "png"
    } else {
        "jpg"
    };
    let part = multipart::Part::bytes(image.to_vec())
        .file_name(format!("shadow-visual.{extension}"))
        .mime_str(media_type)
        .map_err(|_| InferRuntimeClientError::InvalidMediaType)?;
    Ok(multipart::Form::new()
        .text("model", model)
        .text("source_revision", source_revision.to_owned())
        .part("image", part))
}

fn read_bounded(reader: &mut impl Read) -> Result<Vec<u8>, InferRuntimeClientError> {
    let mut bytes = Vec::new();
    reader
        .take((MAX_RESPONSE_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(InferRuntimeClientError::ReadResponse)?;
    if bytes.len() > MAX_RESPONSE_BYTES {
        return Err(InferRuntimeClientError::ResponseTooLarge);
    }
    Ok(bytes)
}

#[derive(Deserialize)]
struct ErrorEnvelope {
    error: ErrorBody,
}

#[derive(Deserialize)]
struct ErrorBody {
    code: String,
}

#[derive(Deserialize)]
struct RawFaceDetectionResponse {
    object: String,
    status: String,
    source_revision: String,
    image: RawImageGeometry,
    detections: Vec<DetectedFace>,
    provenance: VisionProvenance,
}

impl RawFaceDetectionResponse {
    fn validate(
        self,
        expected_source_revision: &str,
    ) -> Result<DetectedFaceBatch, InferRuntimeClientError> {
        if self.object != "vision.face_detection"
            || self.status != "completed"
            || self.source_revision != expected_source_revision
            || self.image.orientation != EXPECTED_ORIENTATION
            || self.image.width == 0
            || self.image.height == 0
            || self.detections.len() > MAX_DETECTIONS
            || !valid_provenance(&self.provenance)
            || self
                .detections
                .iter()
                .any(|detection| !valid_detection(detection, self.image.width, self.image.height))
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "face detection response violated the typed contract",
            ));
        }
        Ok(DetectedFaceBatch {
            source_revision: self.source_revision,
            width: self.image.width,
            height: self.image.height,
            orientation: self.image.orientation,
            detections: self.detections,
            provenance: self.provenance,
        })
    }
}

#[derive(Deserialize)]
struct RawImageGeometry {
    width: u32,
    height: u32,
    orientation: String,
}

#[derive(Deserialize)]
struct RawFaceEmbeddingResponse {
    object: String,
    status: String,
    source_revision: String,
    data_classification: String,
    embedding: RawFaceEmbedding,
    eligibility: FaceEmbeddingEligibility,
    provenance: VisionProvenance,
}

impl RawFaceEmbeddingResponse {
    fn validate(
        self,
        expected_source_revision: &str,
    ) -> Result<EmbeddedFace, InferRuntimeClientError> {
        if self.object != "vision.face_embedding"
            || self.status != "completed"
            || self.source_revision != expected_source_revision
            || self.data_classification != BIOMETRIC_CLASSIFICATION
            || self.embedding.dimensions != crate::SFACE_EMBEDDING_DIMENSIONS
            || !self.embedding.normalized
            || self.embedding.distance_metric != "cosine"
            || !self.eligibility.eligible
            || !self.eligibility.landmarks_in_image
            || !self.eligibility.inter_eye_distance_pixels.is_finite()
            || self.eligibility.inter_eye_distance_pixels < 0.0
            || !self.eligibility.alignment_rmse_pixels.is_finite()
            || self.eligibility.alignment_rmse_pixels < 0.0
            || !valid_provenance(&self.provenance)
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "face embedding response violated the typed contract",
            ));
        }
        let embedding =
            FaceEmbedding::new(self.embedding.values, self.embedding.space).map_err(|_| {
                InferRuntimeClientError::InvalidResponse(
                    "face embedding values violated the SFace contract",
                )
            })?;
        Ok(EmbeddedFace {
            source_revision: self.source_revision,
            embedding,
            eligibility: self.eligibility,
            provenance: self.provenance,
        })
    }
}

#[derive(Deserialize)]
struct RawFaceEmbedding {
    values: Vec<f32>,
    dimensions: usize,
    normalized: bool,
    distance_metric: String,
    space: String,
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
                && f64::from(point.x) < width
                && f64::from(point.y) < height
        })
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

#[derive(Debug, Error)]
pub enum InferRuntimeClientError {
    #[error("infer-runtime base URL must be a path-free literal loopback HTTP URL")]
    InvalidBaseUrl,
    #[error("infer-runtime credential is malformed")]
    InvalidCredential,
    #[error("infer-runtime credential path is not a regular owner file: {0}")]
    UnsafeCredentialFile(PathBuf),
    #[error("infer-runtime credential file is accessible by group or other users: {0}")]
    UnsafeCredentialPermissions(PathBuf),
    #[error("cannot read infer-runtime credential at {path}: {source}")]
    CredentialIo {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("cannot construct the infer-runtime HTTP client: {0}")]
    ClientBuild(reqwest::Error),
    #[error("infer-runtime image length must be in 1..={MAX_IMAGE_BYTES}, got {0}")]
    InvalidImageLength(usize),
    #[error("infer-runtime typed vision accepts only image/jpeg or image/png")]
    InvalidMediaType,
    #[error("infer-runtime source revision must be non-empty and at most 256 bytes")]
    InvalidSourceRevision,
    #[error("infer-runtime semantic text length must be in 1..=4096 bytes, got {0}")]
    InvalidSemanticTextLength(usize),
    #[error("infer-runtime query revision must be non-empty and at most 256 bytes")]
    InvalidQueryRevision,
    #[error("infer-runtime language must be a BCP-47-shaped ASCII tag of at most 35 bytes")]
    InvalidLanguage,
    #[error("infer-runtime classification taxonomy or category set is invalid")]
    InvalidClassificationCategories,
    #[error("cannot serialize infer-runtime request: {0}")]
    SerializeRequest(serde_json::Error),
    #[error("infer-runtime request failed: {0}")]
    Request(reqwest::Error),
    #[error("infer-runtime returned HTTP {status} with code {code}")]
    Http { status: u16, code: String },
    #[error("infer-runtime response exceeded {MAX_RESPONSE_BYTES} bytes")]
    ResponseTooLarge,
    #[error("cannot read infer-runtime response: {0}")]
    ReadResponse(io::Error),
    #[error("cannot decode infer-runtime response: {0}")]
    DecodeResponse(serde_json::Error),
    #[error("invalid infer-runtime response: {0}")]
    InvalidResponse(&'static str),
}

#[cfg(test)]
mod tests;

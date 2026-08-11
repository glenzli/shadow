//! Platform/provider adapters that expose honest availability and execution.
//!
//! The current Apple Vision owner is a non-linking skeleton. It validates the
//! runtime boundary and returns an explicit unavailable terminal until a native
//! Vision bridge supplies real feature-print distances.
//! SAM 2.1 Core ML remains an explicitly installed local process and model
//! route; this crate links neither the native framework nor model weights.

mod apple_vision;
mod infer_runtime;
mod local_process;
mod rawnind_foundation_sidecar;
mod sam2_coreml_sidecar;

pub use apple_vision::AppleVisionFeaturePrintProvider;
pub use infer_runtime::{
    ClassificationReviewCategory, ClassificationReviewDisposition, ClassificationReviewEvidence,
    ClassificationReviewProvider, ClassificationReviewRequest, ClassificationReviewSuggestion,
    DetectedFace, DetectedFaceBatch, EmbeddedFace, FaceAnalysisProvider, FaceEmbeddingEligibility,
    ImageEmbeddingEvidence, ImageUnderstandingEvidence, ImageUnderstandingProvenance,
    ImageUnderstandingProvider, ImageUnderstandingQuality, InferRawFoundationArtifactReceipt,
    InferRawFoundationCancellation, InferRawFoundationDecoderIdentity, InferRawFoundationJob,
    InferRawFoundationLeaseGrant, InferRawFoundationPriority, InferRawFoundationProvenance,
    InferRawFoundationProvider, InferRawFoundationRegisteredLease, InferRawFoundationRequest,
    InferRawFoundationResult, InferRawFoundationSource, InferRawFoundationStaging,
    InferRuntimeClient, InferRuntimeClientError, InferRuntimeCredential, SemanticEmbeddingProvider,
    SemanticRequestPriority, TextEmbeddingEvidence, VisionProvenance, VisionTokenizerProvenance,
};
pub(crate) use rawnind_foundation_sidecar::descriptor_from_planned_verification;
pub use rawnind_foundation_sidecar::{
    RAWNIND_FOUNDATION_ADAPTER_REVISION, RAWNIND_FOUNDATION_ARTIFACT_SET_BLAKE3,
    RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256, RAWNIND_FOUNDATION_IMPLEMENTATION_REVISION,
    RAWNIND_FOUNDATION_MODEL_ID, RAWNIND_FOUNDATION_MODEL_RECEIPT_PREFIX,
    RAWNIND_FOUNDATION_MODEL_REVISION, RAWNIND_FOUNDATION_PACKAGE_SHA256,
    RAWNIND_FOUNDATION_PLAN_RECEIPT_PREFIX, RAWNIND_FOUNDATION_PROVIDER_ID,
    RAWNIND_FOUNDATION_RECEIPT_PREFIX, RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
    RawNindFoundationInput, RawNindFoundationModelVerificationError, RawNindFoundationPlan,
    RawNindFoundationPlanningError, RawNindFoundationProvider,
    RawNindFoundationProviderConfigurationError, VerifiedRawNindFoundationInstallation,
    plan_rawnind_foundation, verify_rawnind_foundation_installation,
};
pub use sam2_coreml_sidecar::{
    SAM2_COREML_ADAPTER_REVISION, SAM2_COREML_ARTIFACT_SET_BLAKE3, SAM2_COREML_EXACT_REVISION,
    SAM2_COREML_MAX_PROMPT_POINTS, SAM2_COREML_MODEL_ID, SAM2_COREML_MODEL_RECEIPT_PREFIX,
    SAM2_COREML_PROVIDER_ID, SAM2_COREML_RECEIPT_PREFIX, Sam2CoreMlModelVerificationError,
    Sam2CoreMlProviderConfigurationError, Sam2CoreMlResidentSession, Sam2CoreMlSidecarProvider,
    VerifiedSam2CoreMlInstallation, verify_sam2_coreml_installation,
};

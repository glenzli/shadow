//! Platform/provider adapters that expose honest availability and execution.
//!
//! The current Apple Vision owner is a non-linking skeleton. It validates the
//! runtime boundary and returns an explicit unavailable terminal until a native
//! Vision bridge supplies real feature-print distances.
//! Shadow's product subject-mask route is the typed local Infer Runtime
//! consumer. This crate links neither a native model framework nor model
//! weights.

mod apple_vision;
mod infer_runtime;

pub use apple_vision::AppleVisionFeaturePrintProvider;
pub use infer_runtime::{
    ClassificationReviewCategory, ClassificationReviewDisposition, ClassificationReviewEvidence,
    ClassificationReviewProvider, ClassificationReviewRequest, ClassificationReviewSuggestion,
    DetectedFace, DetectedFaceBatch, EmbeddedFace, FaceAnalysisProvider, FaceEmbeddingEligibility,
    INFER_IMAGE_COMPLETION_CAPABILITY, INFER_SEMANTIC_GROUNDING_CAPABILITY,
    INFER_SUBJECT_MASK_CAPABILITY, ImageEmbeddingEvidence, ImageUnderstandingEvidence,
    ImageUnderstandingProvenance, ImageUnderstandingProvider, ImageUnderstandingQuality,
    InferImageCompletionEvidence, InferRawFoundationArtifactReceipt,
    InferRawFoundationCancellation, InferRawFoundationDecoderIdentity, InferRawFoundationJob,
    InferRawFoundationLeaseGrant, InferRawFoundationPriority, InferRawFoundationProvenance,
    InferRawFoundationProvider, InferRawFoundationRegisteredLease, InferRawFoundationRequest,
    InferRawFoundationResult, InferRawFoundationSource, InferRawFoundationStaging,
    InferRuntimeAttemptSnapshot, InferRuntimeCancelResult, InferRuntimeCapabilityCatalog,
    InferRuntimeClient, InferRuntimeClientError, InferRuntimeContract, InferRuntimeExplainResult,
    InferRuntimeJobListPage, InferRuntimeJobSnapshot, InferSubjectMaskEvidence, ParsedFace,
    RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256, RAWNIND_FOUNDATION_MODEL_ID,
    RAWNIND_FOUNDATION_PACKAGE_SHA256, RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
    SemanticEmbeddingProvider, SemanticGroundedRegion, SemanticGroundingEvidence,
    SemanticRequestPriority, TextEmbeddingEvidence, VisionProvenance, VisionTokenizerProvenance,
    infer_raw_foundation_sdk_status,
};

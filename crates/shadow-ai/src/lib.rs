//! Model-independent contracts for Shadow's local AI workers and preference learning.
//!
//! This crate deliberately contains no linked model inference runtime,
//! downloaded weights, or image preprocessing implementation. It owns runtime
//! protocols, stable data contracts, and deterministic policy code without
//! pretending that an unavailable provider produced inference.

mod contract;
mod culling;
mod derived_raster;
mod feedback;
mod generated;
mod manifest;
mod people;
mod preference;
mod providers;
mod raw_foundation;
mod remote;
mod resource;
mod runtime;
mod score;
mod semantic;
mod technical;
mod value;
mod wire_v1;

pub use contract::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AI_OBSERVATION_CONTRACT_VERSION, AiCapability, AiJobRequest,
    AiObservation, AiObservationContractError, AiTaskKind, ArtifactReference, ExplanationSignal,
    InputRole, MAX_EXPLANATION_SIGNALS, ObservationTarget, PrivacyClass, ProposalReviewLevel,
    TaskPriority,
};
pub use culling::{
    FeaturePrintCandidate, FeaturePrintDistance, FeaturePrintDistanceBatch,
    FeaturePrintDistanceValue, MAX_FEATURE_PRINT_CANDIDATES, RepresentativeSuggestionBasis,
    SimilarityEvidenceError, SimilarityGroupingPolicy, SimilarityReviewGroup, SimilarityReviewPlan,
    SimilarityReviewStart, propose_similarity_review,
};
pub use derived_raster::{
    DerivedRasterPromotionError, DerivedRasterPromotionFailure, DerivedRasterPromotionRequest,
    MANAGED_DERIVED_RASTER_RECORD_VERSION, ManagedDerivedRaster, ManagedDerivedRasterStore,
    ManagedDerivedStoreCommit, ManagedDerivedStoreRead, ManagedDerivedStoreWrite,
    ManagedGeneratedArtifactReference, PersistedManagedDerivedRaster, promote_derived_raster,
    reload_managed_derived_raster,
};
pub use feedback::{
    BatchBuildReport, FeatureSnapshotRef, FeedbackAction, FeedbackEvent, FeedbackForgetFact,
    FeedbackIgnored, FeedbackValidationError, IncrementalTrainingBatch, IncrementalTrainingPolicy,
    LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PairwiseExampleRef, PairwiseOutcome,
    PresentationContext, PresentedCandidate, PresentedFitMode, PresentedVisualArtifact,
    PresentedVisualFrame, PresentedVisualProvenance, PresentedVisualRole, SuggestionDecision,
    build_incremental_preference_batch,
};
pub use generated::{
    AI_GENERATED_ARTIFACT_CONTRACT_VERSION, AiArtifactContractError, AiGeneratedPayload,
    AiTaskParameters, ArtifactHashAlgorithm, DenoiseDomainPolicy, DenoiseParameters,
    DenoiseQuality, DenoisedRasterArtifact, GeneratedArtifactReference, MAX_MASK_PROMPT_POINTS,
    MAX_RASTER_DIMENSION, MaskPointPolarity, MaskPrompt, MaskPromptPoint, MaskSemantic,
    NormalizedMaskBox, RasterExtent, RasterPixelLayout, RasterSampleFormat, SoftMaskArtifact,
    SoftMaskEncoding, SubjectMaskParameters, TileContract,
};
pub use manifest::{
    BackendRequirement, Dimension, DistributionTerms, ElementType, LicensePermission, LicenseTerms,
    MODEL_MANIFEST_SCHEMA_VERSION, ModelAccess, ModelArtifact, ModelArtifactRole, ModelArtifactSet,
    ModelFormat, ModelManifest, ModelManifestError, NumericRange, Quantization, TensorContract,
    TensorLayout, TensorSemantics,
};
pub use people::{
    ANONYMOUS_PEOPLE_GROUPING_REVISION, AnonymousPeopleGroupingError, AnonymousPeopleGroupingPlan,
    AnonymousPersonGroup, AnonymousPersonGroupingPolicy, FaceBoundingBox, FaceEmbedding,
    FaceLandmarks, FaceOccurrenceEvidence, FaceOccurrenceId, FaceOccurrenceReference, FacePoint,
    MAX_FACE_OCCURRENCES, SFACE_EMBEDDING_DIMENSIONS, propose_anonymous_people,
};
pub use preference::{
    FeatureSchema, FeatureVector, LinearPreferenceHead, PreferenceExample, PreferenceModelError,
    TrainingHyperparameters, TrainingUpdate,
};
pub use providers::{
    AppleVisionFeaturePrintProvider, ClassificationReviewCategory, ClassificationReviewDisposition,
    ClassificationReviewEvidence, ClassificationReviewProvider, ClassificationReviewRequest,
    ClassificationReviewSuggestion, DetectedFace, DetectedFaceBatch, EmbeddedFace,
    FaceAnalysisProvider, FaceEmbeddingEligibility, INFER_SEMANTIC_GROUNDING_CAPABILITY,
    INFER_SUBJECT_MASK_CAPABILITY, ImageEmbeddingEvidence, ImageUnderstandingEvidence,
    ImageUnderstandingProvenance, ImageUnderstandingProvider, ImageUnderstandingQuality,
    InferRawFoundationArtifactReceipt, InferRawFoundationCancellation,
    InferRawFoundationDecoderIdentity, InferRawFoundationJob, InferRawFoundationLeaseGrant,
    InferRawFoundationPriority, InferRawFoundationProvenance, InferRawFoundationProvider,
    InferRawFoundationRegisteredLease, InferRawFoundationRequest, InferRawFoundationResult,
    InferRawFoundationSource, InferRawFoundationStaging, InferRuntimeAttemptSnapshot,
    InferRuntimeCancelResult, InferRuntimeCapabilityCatalog, InferRuntimeClient,
    InferRuntimeClientError, InferRuntimeContract, InferRuntimeExplainResult,
    InferRuntimeJobListPage, InferRuntimeJobSnapshot, InferSubjectMaskEvidence, ParsedFace,
    RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256, RAWNIND_FOUNDATION_MODEL_ID,
    RAWNIND_FOUNDATION_PACKAGE_SHA256, RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
    SemanticEmbeddingProvider, SemanticGroundedRegion, SemanticGroundingEvidence,
    SemanticRequestPriority, TextEmbeddingEvidence, VisionProvenance, VisionTokenizerProvenance,
    infer_raw_foundation_sdk_status,
};
pub use raw_foundation::{
    MAX_RAW_FOUNDATION_IMPLEMENTATION_REVISION_BYTES, RAW_FOUNDATION_ENCODING_VERSION,
    RAW_FOUNDATION_MEDIA_TYPE, RawFoundationArtifact, RawFoundationArtifactError,
    RawFoundationDigestField, RawFoundationMaterializationDisposition, RawFoundationProvenance,
    RawFoundationSourceProvenance,
};
pub use remote::{
    MAX_REMOTE_INPUTS, PREPARED_REMOTE_UPLOAD_CONTRACT_VERSION, PreparedRemoteUpload,
    PreparedRemoteUploadCommit, PreparedRemoteUploadError, REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION,
    REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION, RemoteDataRetention, RemoteExecutionContext,
    RemoteExecutionDenied, RemoteExecutionGrant, RemoteExecutionPolicy, RemoteOfflineBehavior,
    RemoteProviderBlocker, RemoteProviderManifest, RemoteProviderManifestError, RemoteTrainingUse,
    RemoteTransportExecution, RemoteUploadPreparation, RemoteUploadPreparationFailure,
    RemoteUploadScope, RemoteUploadStore, admit_remote_execution, prepare_remote_upload,
};
pub use resource::{
    AdmissionBlocker, AdmissionDecision, AdmissionRequest, BackendKind, ExecutionBackend,
    HardwareProfile, LocalModelAvailability, ModelAvailability, NumericPrecision, OnBatteryPolicy,
    ResourceEstimate, ResourcePolicy, RunPlan, admit,
};
pub use runtime::{
    AdmittedExecution, AdmittedModelIdentity, CancellationToken,
    EXECUTION_PLAN_IDENTITY_CONTRACT_VERSION, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionLease, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    LeaseBoundOutput, LocalExecutionAdmission, LocalExecutionBinding,
    MODEL_PROVENANCE_CONTRACT_VERSION, ModelProvenance, ProviderExecutionClass, ProviderIdentity,
    ProviderTerminal, ProviderUnavailable, RUNTIME_PROGRESS_COMPLETE, RuntimeContractError,
    RuntimeFailure, RuntimeProgress, RuntimeProgressError, RuntimeProgressReporter,
    RuntimeProgressSink, RuntimeProvider, RuntimeTerminalOutcome, RuntimeTerminalReceipt,
    RuntimeUsage, admit_local_execution, bind_local_service_execution, bind_system_execution,
};
pub use score::{
    CandidateAssessment, CandidateRank, CandidateSignals, DefectGate, PersonalPreferenceSignal,
    ScoreContribution, ScoredSignal, SelectionPolicy, SelectionWeights, VisibilityDisposition,
    rank_group,
};
pub use semantic::{
    MAX_LANGUAGE_TAG_BYTES, MAX_SEMANTIC_EMBEDDING_DIMENSIONS, MAX_SEMANTIC_ID_BYTES,
    MAX_SEMANTIC_KEYWORD_SUGGESTIONS, MAX_SEMANTIC_LABEL_BYTES, MAX_SHORT_CAPTION_BYTES,
    SEMANTIC_EMBEDDING_CONTRACT_VERSION, SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
    SemanticContractError, SemanticEmbedding, SemanticEmbeddingSpace, SemanticEvidenceKind,
    SemanticImageAnalysis, SemanticKeywordKind, SemanticKeywordSuggestion, SemanticShortCaption,
};
pub use technical::{
    DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, LUMA_HISTOGRAM_BIN_COUNT, LumaHistogram,
    MAX_ACTIVE_LUMA_SAMPLES, MAX_DISPLAY_LUMA_BUFFER_SAMPLES, MAX_DISPLAY_LUMA_DIMENSION,
    NEAR_BLACK_LUMA_THRESHOLD, NEAR_WHITE_LUMA_THRESHOLD, NonNegativeFinite,
    TECHNICAL_QUALITY_IMPLEMENTATION_VERSION, TECHNICAL_QUALITY_SCHEMA_VERSION,
    TechnicalAlgorithmProvenance, TechnicalInputProvenance, TechnicalObservationError,
    TechnicalQualityMetrics, TechnicalQualityObservation, observe_display_luma,
};
pub use value::{UnitInterval, UnitIntervalError};

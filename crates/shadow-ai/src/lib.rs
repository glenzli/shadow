//! Model-independent contracts for Shadow's local AI workers and preference learning.
//!
//! This crate deliberately contains no model runtime, downloaded weights, image
//! preprocessing implementation, or pretend inference. It owns stable data
//! contracts and deterministic policy code that remain useful while concrete
//! models and execution providers change.

mod contract;
mod feedback;
mod generated;
mod manifest;
mod preference;
mod resource;
mod score;
mod technical;
mod value;

pub use contract::{
    AiCapability, AiJobRequest, AiObservation, AiTaskKind, ArtifactReference, ExplanationSignal,
    InputRole, ModelProvenance, ObservationTarget, PrivacyClass, ProposalReviewLevel, TaskPriority,
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
    DenoiseQuality, DenoisedRasterArtifact, GeneratedArtifactReference,
    GeneratedArtifactStorageClass, MAX_MASK_PROMPT_POINTS, MAX_RASTER_DIMENSION, MaskPointPolarity,
    MaskPrompt, MaskPromptPoint, MaskSemantic, NormalizedMaskBox, RasterExtent, RasterPixelLayout,
    RasterSampleFormat, SoftMaskArtifact, SoftMaskEncoding, SubjectMaskParameters, TileContract,
};
pub use manifest::{
    BackendRequirement, Dimension, DistributionTerms, ElementType, LicensePermission, LicenseTerms,
    ModelAccess, ModelArtifact, ModelFormat, ModelManifest, ModelManifestError, NumericRange,
    Quantization, TensorContract, TensorLayout, TensorSemantics,
};
pub use preference::{
    FeatureSchema, FeatureVector, LinearPreferenceHead, PreferenceExample, PreferenceModelError,
    TrainingHyperparameters, TrainingUpdate,
};
pub use resource::{
    AdmissionBlocker, AdmissionDecision, AdmissionRequest, BackendKind, ExecutionBackend,
    HardwareProfile, LocalModelAvailability, ModelAvailability, NumericPrecision, OnBatteryPolicy,
    RemoteExecutionPolicy, RemoteModelAvailability, ResourceEstimate, ResourcePolicy, RunPlan,
    admit,
};
pub use score::{
    CandidateAssessment, CandidateRank, CandidateSignals, DefectGate, PersonalPreferenceSignal,
    ScoreContribution, ScoredSignal, SelectionPolicy, SelectionWeights, VisibilityDisposition,
    rank_group,
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

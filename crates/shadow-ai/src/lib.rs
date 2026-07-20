//! Model-independent contracts for Shadow's local AI workers and preference learning.
//!
//! This crate deliberately contains no model runtime, downloaded weights, image
//! preprocessing implementation, or pretend inference. It owns stable data
//! contracts and deterministic policy code that remain useful while concrete
//! models and execution providers change.

mod contract;
mod feedback;
mod manifest;
mod preference;
mod resource;
mod score;
mod value;

pub use contract::{
    AiCapability, AiJobRequest, AiObservation, AiTaskKind, ArtifactReference, ExplanationSignal,
    InputRole, ModelProvenance, ObservationTarget, PrivacyClass, ProposalReviewLevel, TaskPriority,
};
pub use feedback::{
    BatchBuildReport, FeatureSnapshotRef, FeedbackAction, FeedbackEvent, FeedbackForgetFact,
    FeedbackIgnored, FeedbackValidationError, IncrementalTrainingBatch, IncrementalTrainingPolicy,
    LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PairwiseExampleRef, PairwiseOutcome,
    PresentationContext, PresentedCandidate, SuggestionDecision,
    build_incremental_preference_batch,
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
pub use value::{UnitInterval, UnitIntervalError};

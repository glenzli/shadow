//! Human feedback evidence and deterministic preference-training admission.
//!
//! Start with [`FeedbackEvent`] for the append-only evidence contract and
//! [`build_incremental_preference_batch`] for the explicit-only learning boundary.

mod evidence;
#[cfg(test)]
mod test_support;
mod training;

pub use evidence::{
    FeatureSnapshotRef, FeedbackAction, FeedbackEvent, FeedbackForgetFact, FeedbackValidationError,
    LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PairwiseOutcome, PresentationContext,
    PresentedCandidate, PresentedFitMode, PresentedVisualArtifact, PresentedVisualFrame,
    PresentedVisualProvenance, PresentedVisualRole, SuggestionDecision,
};
pub use training::{
    BatchBuildReport, FeedbackIgnored, IncrementalTrainingBatch, IncrementalTrainingPolicy,
    PairwiseExampleRef, build_incremental_preference_batch,
};

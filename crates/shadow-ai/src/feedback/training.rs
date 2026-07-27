//! Deterministic admission of explicit feedback into incremental preference batches.

use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};
use shadow_domain::PhotoId;

use super::evidence::{
    FeatureSnapshotRef, FeedbackAction, FeedbackEvent, LearningScope, PairwiseOutcome,
};

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PairwiseExampleRef {
    pub event_id: String,
    pub sequence: u64,
    pub scope: LearningScope,
    pub preferred_photo: PhotoId,
    pub other_photo: PhotoId,
    pub preferred_feature: FeatureSnapshotRef,
    pub other_feature: FeatureSnapshotRef,
    /// Explicit pairwise choices are `1.0`; future weaker evidence must be lower.
    pub weight_micros: u32,
}

impl PairwiseExampleRef {
    pub fn weight(&self) -> f64 {
        f64::from(self.weight_micros) / 1_000_000.0
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct IncrementalTrainingBatch {
    pub scope: LearningScope,
    pub after_sequence_exclusive: u64,
    pub through_sequence_inclusive: u64,
    pub examples: Vec<PairwiseExampleRef>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct IncrementalTrainingPolicy {
    pub scope: LearningScope,
    pub learning_paused: bool,
    pub after_sequence_exclusive: u64,
    pub maximum_examples: usize,
    /// Forgetting is non-destructive: source events remain human history but stop training.
    pub forgotten_event_ids: BTreeSet<String>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "reason")]
pub enum FeedbackIgnored {
    LearningPaused,
    AlreadyConsumed { event_id: String },
    Forgotten { event_id: String },
    OutsideLearningScope { event_id: String },
    NotExplicitPairwise { event_id: String },
    NonDecisivePairwise { event_id: String },
    CandidateWasNotPresented { event_id: String, photo_id: PhotoId },
    MissingFrozenFeature { event_id: String, photo_id: PhotoId },
    FeatureSchemaMismatch { event_id: String },
    BatchLimitReached { event_id: String },
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct BatchBuildReport {
    pub batch: IncrementalTrainingBatch,
    pub ignored: Vec<FeedbackIgnored>,
}

/// Builds deterministic training references from explicit pairwise comparisons only.
///
/// Picks, exports, dwell time, and lack of clicks remain recorded feedback but
/// are deliberately not converted to hidden negative examples here.
pub fn build_incremental_preference_batch(
    events: &[FeedbackEvent],
    policy: &IncrementalTrainingPolicy,
) -> BatchBuildReport {
    let mut ordered: Vec<_> = events.iter().collect();
    ordered.sort_by(|left, right| {
        left.sequence
            .cmp(&right.sequence)
            .then_with(|| left.event_id.cmp(&right.event_id))
    });
    let mut report = BatchBuildReport {
        batch: IncrementalTrainingBatch {
            scope: policy.scope.clone(),
            after_sequence_exclusive: policy.after_sequence_exclusive,
            through_sequence_inclusive: policy.after_sequence_exclusive,
            examples: Vec::new(),
        },
        ignored: Vec::new(),
    };

    if policy.learning_paused {
        report.ignored.push(FeedbackIgnored::LearningPaused);
        return report;
    }

    for event in ordered {
        if event.sequence <= policy.after_sequence_exclusive {
            report.ignored.push(FeedbackIgnored::AlreadyConsumed {
                event_id: event.event_id.clone(),
            });
            continue;
        }
        if report.batch.examples.len() >= policy.maximum_examples {
            report.ignored.push(FeedbackIgnored::BatchLimitReached {
                event_id: event.event_id.clone(),
            });
            break;
        }
        report.batch.through_sequence_inclusive =
            report.batch.through_sequence_inclusive.max(event.sequence);
        if policy.forgotten_event_ids.contains(&event.event_id) {
            report.ignored.push(FeedbackIgnored::Forgotten {
                event_id: event.event_id.clone(),
            });
            continue;
        }
        if event.scope != policy.scope {
            report.ignored.push(FeedbackIgnored::OutsideLearningScope {
                event_id: event.event_id.clone(),
            });
            continue;
        }
        match example_from_event(event) {
            Ok(example) => report.batch.examples.push(example),
            Err(ignored) => report.ignored.push(ignored),
        }
    }
    report
}

fn example_from_event(event: &FeedbackEvent) -> Result<PairwiseExampleRef, FeedbackIgnored> {
    let FeedbackAction::PairwiseComparison {
        left,
        right,
        outcome,
    } = event.action
    else {
        return Err(FeedbackIgnored::NotExplicitPairwise {
            event_id: event.event_id.clone(),
        });
    };
    let (preferred, other) = match outcome {
        PairwiseOutcome::LeftPreferred => (left, right),
        PairwiseOutcome::RightPreferred => (right, left),
        PairwiseOutcome::KeepBoth
        | PairwiseOutcome::KeepNeither
        | PairwiseOutcome::CannotCompare => {
            return Err(FeedbackIgnored::NonDecisivePairwise {
                event_id: event.event_id.clone(),
            });
        }
    };
    let preferred_feature = feature_for(event, preferred)?;
    let other_feature = feature_for(event, other)?;
    if preferred_feature.extractor_id != other_feature.extractor_id
        || preferred_feature.extractor_revision != other_feature.extractor_revision
        || preferred_feature.preprocessing_version != other_feature.preprocessing_version
        || preferred_feature.dimension != other_feature.dimension
    {
        return Err(FeedbackIgnored::FeatureSchemaMismatch {
            event_id: event.event_id.clone(),
        });
    }
    Ok(PairwiseExampleRef {
        event_id: event.event_id.clone(),
        sequence: event.sequence,
        scope: event.scope.clone(),
        preferred_photo: preferred,
        other_photo: other,
        preferred_feature,
        other_feature,
        weight_micros: 1_000_000,
    })
}

fn feature_for(
    event: &FeedbackEvent,
    photo_id: PhotoId,
) -> Result<FeatureSnapshotRef, FeedbackIgnored> {
    let Some(candidate) = event
        .presentation
        .candidates
        .iter()
        .find(|candidate| candidate.photo_id == photo_id)
    else {
        return Err(FeedbackIgnored::CandidateWasNotPresented {
            event_id: event.event_id.clone(),
            photo_id,
        });
    };
    candidate
        .feature
        .clone()
        .ok_or_else(|| FeedbackIgnored::MissingFrozenFeature {
            event_id: event.event_id.clone(),
            photo_id,
        })
}

#[cfg(test)]
#[path = "training_tests.rs"]
mod tests;

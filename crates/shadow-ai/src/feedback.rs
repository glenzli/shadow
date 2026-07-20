use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Serialize};
use shadow_domain::{GroupId, PhotoId, RecipeCommitId};

use crate::{ModelProvenance, UnitInterval};

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum LearningScope {
    Global,
    Project { project_id: String },
}

/// Identity of a frozen feature artifact. Vectors themselves remain rebuildable data.
#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
pub struct FeatureSnapshotRef {
    pub extractor_id: String,
    pub extractor_revision: String,
    pub preprocessing_version: String,
    pub artifact_hash: String,
    pub dimension: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PresentedCandidate {
    pub photo_id: PhotoId,
    pub position: u32,
    pub visible_fraction: UnitInterval,
    pub inspected_at_one_to_one: bool,
    pub feature: Option<FeatureSnapshotRef>,
}

/// What the user could actually see when an action occurred.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PresentationContext {
    pub session_id: String,
    pub group_id: Option<GroupId>,
    pub candidates: Vec<PresentedCandidate>,
    pub active_model: Option<ModelProvenance>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PairwiseOutcome {
    LeftPreferred,
    RightPreferred,
    KeepBoth,
    KeepNeither,
    CannotCompare,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SuggestionDecision {
    Accepted,
    Rejected,
    AcceptedThenUndone,
}

/// Append-only human evidence. Absence of an event is never a negative label.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "action")]
pub enum FeedbackAction {
    PairwiseComparison {
        left: PhotoId,
        right: PhotoId,
        outcome: PairwiseOutcome,
    },
    FlagChanged {
        photo_id: PhotoId,
        before: Option<String>,
        after: Option<String>,
    },
    RatingChanged {
        photo_id: PhotoId,
        before: Option<u8>,
        after: Option<u8>,
    },
    SuggestionReviewed {
        proposal_id: String,
        decision: SuggestionDecision,
    },
    DevelopProposalEdited {
        proposal_id: String,
        suggested_recipe: RecipeCommitId,
        final_recipe: RecipeCommitId,
        /// Semantic parameter names and normalized final-minus-suggested residuals.
        parameter_residuals: BTreeMap<String, f64>,
    },
    ParametersCopied {
        source_photo: PhotoId,
        target_photos: Vec<PhotoId>,
    },
    Exported {
        photo_id: PhotoId,
    },
    ReturnedForRework {
        photo_id: PhotoId,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FeedbackEvent {
    pub event_id: String,
    /// Catalog-assigned monotonic sequence; wall-clock time is not ordering truth.
    pub sequence: u64,
    pub occurred_at_unix_ms: i64,
    pub scope: LearningScope,
    pub presentation: PresentationContext,
    pub action: FeedbackAction,
}

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
mod tests {
    use shadow_domain::EntityId;

    use super::*;

    fn candidate(photo_id: PhotoId, artifact_hash: &str) -> PresentedCandidate {
        PresentedCandidate {
            photo_id,
            position: 0,
            visible_fraction: UnitInterval::ONE,
            inspected_at_one_to_one: false,
            feature: Some(FeatureSnapshotRef {
                extractor_id: "frozen-image-features".into(),
                extractor_revision: "r1".into(),
                preprocessing_version: "preview-v1".into(),
                artifact_hash: artifact_hash.into(),
                dimension: 2,
            }),
        }
    }

    fn event(action: FeedbackAction) -> FeedbackEvent {
        FeedbackEvent {
            event_id: "event-1".into(),
            sequence: 1,
            occurred_at_unix_ms: 123,
            scope: LearningScope::Global,
            presentation: PresentationContext {
                session_id: "review-1".into(),
                group_id: None,
                candidates: vec![],
                active_model: None,
            },
            action,
        }
    }

    fn policy() -> IncrementalTrainingPolicy {
        IncrementalTrainingPolicy {
            scope: LearningScope::Global,
            learning_paused: false,
            after_sequence_exclusive: 0,
            maximum_examples: 100,
            forgotten_event_ids: BTreeSet::new(),
        }
    }

    #[test]
    fn explicit_pairwise_choice_becomes_training_evidence() {
        let left = PhotoId::new_v7();
        let right = PhotoId::new_v7();
        let mut comparison = event(FeedbackAction::PairwiseComparison {
            left,
            right,
            outcome: PairwiseOutcome::RightPreferred,
        });
        comparison.presentation.candidates =
            vec![candidate(left, "left"), candidate(right, "right")];
        let report = build_incremental_preference_batch(&[comparison], &policy());
        assert_eq!(report.batch.examples.len(), 1);
        assert_eq!(report.batch.examples[0].preferred_photo, right);
        assert!((report.batch.examples[0].weight() - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn export_is_not_silently_converted_to_a_negative_pair() {
        let exported = event(FeedbackAction::Exported {
            photo_id: PhotoId::new_v7(),
        });
        let report = build_incremental_preference_batch(&[exported], &policy());
        assert!(report.batch.examples.is_empty());
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::NotExplicitPairwise { .. }]
        ));
    }

    #[test]
    fn forgotten_feedback_stays_out_of_future_batches() {
        let mut forgotten = policy();
        forgotten.forgotten_event_ids.insert("event-1".into());
        let report = build_incremental_preference_batch(
            &[event(FeedbackAction::Exported {
                photo_id: PhotoId::new_v7(),
            })],
            &forgotten,
        );
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::Forgotten { .. }]
        ));
    }

    #[test]
    fn batch_limit_does_not_advance_past_untrained_evidence() {
        let left = PhotoId::new_v7();
        let right = PhotoId::new_v7();
        let mut first = event(FeedbackAction::PairwiseComparison {
            left,
            right,
            outcome: PairwiseOutcome::LeftPreferred,
        });
        first.presentation.candidates = vec![candidate(left, "left"), candidate(right, "right")];
        let mut second = first.clone();
        second.event_id = "event-2".into();
        second.sequence = 2;
        let mut limited = policy();
        limited.maximum_examples = 1;

        let report = build_incremental_preference_batch(&[second, first], &limited);
        assert_eq!(report.batch.examples.len(), 1);
        assert_eq!(report.batch.through_sequence_inclusive, 1);
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::BatchLimitReached { event_id }] if event_id == "event-2"
        ));
    }

    #[test]
    fn project_feedback_does_not_leak_into_the_global_head() {
        let mut project_event = event(FeedbackAction::Exported {
            photo_id: PhotoId::new_v7(),
        });
        project_event.scope = LearningScope::Project {
            project_id: "wedding-1".into(),
        };
        let report = build_incremental_preference_batch(&[project_event], &policy());
        assert!(report.batch.examples.is_empty());
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::OutsideLearningScope { .. }]
        ));
    }
}

use std::collections::BTreeSet;

use shadow_domain::{EntityId, PhotoId};

use super::*;
use crate::{
    UnitInterval,
    feedback::evidence::{
        FeatureSnapshotRef, FeedbackAction, LearningScope, PairwiseOutcome, PresentedCandidate,
    },
    feedback::test_support::event,
};

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
        visual: None,
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
    comparison.presentation.candidates = vec![candidate(left, "left"), candidate(right, "right")];
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

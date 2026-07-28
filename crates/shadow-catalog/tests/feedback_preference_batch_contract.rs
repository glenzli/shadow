mod feedback_contract_support;

use feedback_contract_support::{presentation, register_source};
use shadow_ai::{
    FeatureSnapshotRef, FeedbackAction, IncrementalTrainingPolicy, LearningScope, NewFeedbackEvent,
    PairwiseOutcome, PresentedCandidate, UnitInterval, build_incremental_preference_batch,
};
use shadow_catalog::Catalog;
use shadow_domain::PhotoId;

fn candidate(photo_id: PhotoId, position: u32, hash: &str) -> PresentedCandidate {
    PresentedCandidate {
        photo_id,
        position,
        visible_fraction: UnitInterval::ONE,
        inspected_at_one_to_one: false,
        feature: Some(FeatureSnapshotRef {
            extractor_id: "frozen-features".into(),
            extractor_revision: "r1".into(),
            preprocessing_version: "display-proxy-v1".into(),
            artifact_hash: hash.into(),
            dimension: 3,
        }),
        visual: None,
    }
}

fn pairwise_event(event_id: &str, left: PhotoId, right: PhotoId) -> NewFeedbackEvent {
    NewFeedbackEvent {
        event_id: event_id.into(),
        occurred_at_unix_ms: 1_700_000_001_000,
        scope: LearningScope::Global,
        presentation: presentation(vec![
            candidate(left, 0, "left-feature"),
            candidate(right, 1, "right-feature"),
        ]),
        action: FeedbackAction::PairwiseComparison {
            left,
            right,
            outcome: PairwiseOutcome::LeftPreferred,
        },
    }
}

#[test]
fn persisted_page_feeds_incremental_preference_batch_without_translation() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let left = register_source(&mut catalog, 1).photo_id;
    let right = register_source(&mut catalog, 2).photo_id;
    catalog
        .append_feedback_event(&pairwise_event("pairwise-1", left, right))
        .expect("append pairwise event");
    let page = catalog
        .feedback_events_after(&LearningScope::Global, 0, 100)
        .expect("read training page");
    let policy = IncrementalTrainingPolicy {
        scope: LearningScope::Global,
        learning_paused: false,
        after_sequence_exclusive: 0,
        maximum_examples: 100,
        forgotten_event_ids: catalog
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read forgotten ids"),
    };
    let report = build_incremental_preference_batch(&page.events, &policy);
    assert_eq!(report.batch.examples.len(), 1);
    assert_eq!(report.batch.examples[0].event_id, "pairwise-1");
    assert_eq!(report.batch.examples[0].preferred_photo, left);
}

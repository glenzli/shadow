use std::collections::BTreeMap;

use shadow_ai::{FeedbackAction, LearningScope, NewFeedbackEvent};
use shadow_domain::{EntityId, PhotoId, RecipeCommitId};

use super::{
    super::*,
    event_fixtures::{
        pairwise_event, presentation, presented_visual, register_photo, register_source,
    },
};

#[test]
fn malformed_evidence_and_unknown_photos_fail_before_persistence() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo = register_photo(&mut catalog, 1);

    let mut duplicate_candidate = pairwise_event(
        "duplicate-candidate",
        LearningScope::Global,
        photo,
        PhotoId::new_v7(),
    );
    duplicate_candidate.presentation.candidates[1].photo_id = photo;
    assert!(matches!(
        catalog.append_feedback_event(&duplicate_candidate),
        Err(CatalogError::InvalidFeedback(_))
    ));

    let mut missing_pairwise_candidate = pairwise_event(
        "missing-pairwise-candidate",
        LearningScope::Global,
        photo,
        PhotoId::new_v7(),
    );
    missing_pairwise_candidate.presentation.candidates.pop();
    assert!(matches!(
        catalog.append_feedback_event(&missing_pairwise_candidate),
        Err(CatalogError::InvalidFeedback(_))
    ));

    let invalid_rating = NewFeedbackEvent {
        event_id: "invalid-rating".into(),
        occurred_at_unix_ms: 1,
        scope: LearningScope::Global,
        presentation: presentation(vec![]),
        action: FeedbackAction::RatingChanged {
            photo_id: photo,
            before: Some(5),
            after: Some(6),
        },
    };
    assert!(matches!(
        catalog.append_feedback_event(&invalid_rating),
        Err(CatalogError::InvalidFeedback(_))
    ));

    let invalid_residual = NewFeedbackEvent {
        event_id: "invalid-residual".into(),
        occurred_at_unix_ms: 1,
        scope: LearningScope::Global,
        presentation: presentation(vec![]),
        action: FeedbackAction::DevelopProposalEdited {
            proposal_id: "proposal-1".into(),
            suggested_recipe: RecipeCommitId::new_v7(),
            final_recipe: RecipeCommitId::new_v7(),
            parameter_residuals: BTreeMap::from([("exposure".into(), f64::NAN)]),
        },
    };
    assert!(matches!(
        catalog.append_feedback_event(&invalid_residual),
        Err(CatalogError::InvalidFeedback(_))
    ));

    let unknown_photo = NewFeedbackEvent {
        event_id: "unknown-photo".into(),
        occurred_at_unix_ms: 1_700_000_001_000,
        scope: LearningScope::Global,
        presentation: presentation(vec![]),
        action: FeedbackAction::Exported {
            photo_id: PhotoId::new_v7(),
        },
    };
    assert!(matches!(
        catalog.append_feedback_event(&unknown_photo),
        Err(CatalogError::PhotoNotFound(_))
    ));
    assert!(
        catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read empty feedback")
            .events
            .is_empty()
    );
}

#[test]
fn presented_visual_representation_must_belong_to_its_candidate_photo() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let left = register_source(&mut catalog, 1);
    let right = register_source(&mut catalog, 2);
    let mut request = pairwise_event(
        "cross-owned-visual",
        LearningScope::Global,
        left.photo_id,
        right.photo_id,
    );
    request.presentation.candidates[0].visual = Some(presented_visual(right.representation_id));
    request.presentation.candidates[1].visual = Some(presented_visual(right.representation_id));

    assert!(matches!(
        catalog.append_feedback_event(&request),
        Err(CatalogError::FeedbackVisualRepresentationOwnerMismatch {
            photo_id,
            representation_id,
        }) if photo_id == left.photo_id && representation_id == right.representation_id
    ));
    assert!(
        catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read empty feedback")
            .events
            .is_empty()
    );
}

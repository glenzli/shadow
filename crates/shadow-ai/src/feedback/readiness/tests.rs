use std::collections::BTreeSet;

use super::*;
use crate::feedback::test_support::event;
use crate::{PairwiseOutcome, PresentedCandidate, UnitInterval};
use shadow_domain::{EntityId, PhotoId, RecipeCommitId};

fn approved() -> FeedbackEvent {
    event(FeedbackAction::EditExampleConfirmed {
        photo_id: PhotoId::new_v7(),
        baseline_recipe: RecipeCommitId::new_v7(),
        approved_recipe: RecipeCommitId::new_v7(),
        origin: EditExampleOrigin::Imported,
        intent: EditExampleIntent::PersonalStyle,
    })
}

#[test]
fn approval_retains_origin_and_forgetting_excludes_without_mutating_history() {
    let approval = approved();
    let events = [approval.clone()];
    let report =
        build_learning_readiness(&events, &LearningScope::Global, &BTreeSet::new()).unwrap();
    assert_eq!(
        report.approved_edit_references[0].origin,
        EditExampleOrigin::Imported
    );
    assert!(report.pairwise_references.is_empty());
    let report = build_learning_readiness(
        &events,
        &LearningScope::Global,
        &BTreeSet::from([approval.event_id.clone()]),
    )
    .unwrap();
    assert!(report.approved_edit_references.is_empty());
    assert_eq!(report.excluded[0].reason, "forgotten");
    assert_eq!(events[0], approval);
}

#[test]
fn no_feature_choices_and_exports_do_not_become_training_examples() {
    let left = PhotoId::new_v7();
    let right = PhotoId::new_v7();
    let mut choice = event(FeedbackAction::PairwiseComparison {
        left,
        right,
        outcome: PairwiseOutcome::LeftPreferred,
    });
    choice.presentation.candidates = [left, right]
        .into_iter()
        .enumerate()
        .map(|(position, photo_id)| PresentedCandidate {
            photo_id,
            position: u32::try_from(position).unwrap(),
            visible_fraction: UnitInterval::ONE,
            inspected_at_one_to_one: false,
            feature: None,
            visual: None,
        })
        .collect();
    let mut export = event(FeedbackAction::Exported { photo_id: left });
    export.sequence = 2;
    export.event_id = "export".into();
    let report =
        build_learning_readiness(&[export, choice], &LearningScope::Global, &BTreeSet::new())
            .unwrap();
    assert!(report.pairwise_references.is_empty());
    assert!(report.approved_edit_references.is_empty());
    assert_eq!(report.excluded[0].reason, "missing_frozen_feature");
    assert_eq!(
        report.excluded[1].reason,
        "not_explicit_preference_or_edit_approval"
    );
}

#[test]
fn scope_duplicates_invalid_and_oversized_pages_fail_closed() {
    let approval = approved();
    let report = build_learning_readiness(
        std::slice::from_ref(&approval),
        &LearningScope::Project {
            project_id: "work".into(),
        },
        &BTreeSet::new(),
    )
    .unwrap();
    assert_eq!(report.excluded[0].reason, "outside_scope");
    assert!(
        build_learning_readiness(
            &[approval.clone(), approval.clone()],
            &LearningScope::Global,
            &BTreeSet::new()
        )
        .is_err()
    );
    assert!(
        build_learning_readiness(
            &vec![approval; 513],
            &LearningScope::Global,
            &BTreeSet::new()
        )
        .is_err()
    );
}

#[test]
fn explicit_no_change_approval_is_preserved() {
    let mut approval = approved();
    if let FeedbackAction::EditExampleConfirmed {
        baseline_recipe,
        approved_recipe,
        ..
    } = &mut approval.action
    {
        *approved_recipe = *baseline_recipe;
    }
    let json = serde_json::to_string(&approval).unwrap();
    let restored: FeedbackEvent = serde_json::from_str(&json).unwrap();
    let report =
        build_learning_readiness(&[restored], &LearningScope::Global, &BTreeSet::new()).unwrap();
    assert_eq!(report.approved_edit_references.len(), 1);
}

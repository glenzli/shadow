use super::event_fixtures::{presentation, register_photo};
use crate::{Catalog, CommitRecipe};
use shadow_ai::{
    EditExampleIntent, EditExampleOrigin, FeedbackAction, LearningScope, NewFeedbackEvent,
    NewFeedbackForgetFact, build_learning_readiness,
};
use shadow_domain::{EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId, RecipeSnapshot};

fn save_recipe(catalog: &mut Catalog, photo: PhotoId) -> RecipeCommitId {
    let commit = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        vec![],
        RecipeSnapshot::empty(),
        None,
        1,
    )
    .unwrap();
    let id = commit.id();
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id: photo,
            commit,
            update_refs: vec![],
        })
        .unwrap();
    id
}

#[test]
fn approvals_require_owned_recipes_and_respect_forget_facts() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let photo = register_photo(&mut catalog, 1);
    let other = register_photo(&mut catalog, 2);
    let baseline = save_recipe(&mut catalog, photo);
    let approved = save_recipe(&mut catalog, photo);
    let foreign = save_recipe(&mut catalog, other);
    let mut request = NewFeedbackEvent {
        event_id: "edit-approved".into(),
        occurred_at_unix_ms: 10,
        scope: LearningScope::Global,
        presentation: presentation(vec![]),
        action: FeedbackAction::EditExampleConfirmed {
            photo_id: photo,
            baseline_recipe: baseline,
            approved_recipe: foreign,
            origin: EditExampleOrigin::Assisted,
            intent: EditExampleIntent::TechnicalCorrection,
        },
    };
    assert!(catalog.append_feedback_event(&request).is_err());
    if let FeedbackAction::EditExampleConfirmed {
        approved_recipe, ..
    } = &mut request.action
    {
        *approved_recipe = RecipeCommitId::new_v7();
    }
    assert!(catalog.append_feedback_event(&request).is_err());
    if let FeedbackAction::EditExampleConfirmed {
        approved_recipe, ..
    } = &mut request.action
    {
        *approved_recipe = approved;
    }
    let event = catalog.append_feedback_event(&request).unwrap();
    assert_eq!(event.sequence, 1);
    let ready = catalog
        .learning_evidence_page(&LearningScope::Global, 0, 1)
        .unwrap();
    assert_eq!(ready.report.approved_edit_references.len(), 1);
    assert!(ready.unavailable_edit_event_ids.is_empty());
    assert!(!ready.training_performed);
    assert!(!ready.has_more);
    assert_eq!(ready.next_after_sequence, 1);
    assert!(catalog.append_feedback_event(&request).is_err());
    let page = catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .unwrap();
    assert_eq!(page.events, vec![event.clone()]);
    catalog
        .append_feedback_forget_fact(&NewFeedbackForgetFact {
            fact_id: "forget-edit".into(),
            target_event_id: event.event_id,
            occurred_at_unix_ms: 11,
            reason: None,
        })
        .unwrap();
    let forgotten = catalog
        .forgotten_feedback_event_ids(&LearningScope::Global)
        .unwrap();
    let report =
        build_learning_readiness(&page.events, &LearningScope::Global, &forgotten).unwrap();
    assert!(report.approved_edit_references.is_empty());
    assert!(catalog.recipe_commit(photo, approved).unwrap().is_some());
}

#[test]
fn readiness_pages_do_not_claim_missing_recipes_or_other_scopes_are_ready() {
    use super::event_fixtures::exported_event;
    let mut catalog = Catalog::open_in_memory().unwrap();
    let photo = register_photo(&mut catalog, 3);
    let recipe = save_recipe(&mut catalog, photo);
    catalog
        .append_feedback_event(&NewFeedbackEvent {
            event_id: "approved-no-change".into(),
            occurred_at_unix_ms: 1,
            scope: LearningScope::Global,
            presentation: presentation(vec![]),
            action: FeedbackAction::EditExampleConfirmed {
                photo_id: photo,
                baseline_recipe: recipe,
                approved_recipe: recipe,
                origin: EditExampleOrigin::Manual,
                intent: EditExampleIntent::PersonalStyle,
            },
        })
        .unwrap();
    catalog
        .append_feedback_event(&exported_event(
            "project-only",
            LearningScope::Project {
                project_id: "work".into(),
            },
            photo,
        ))
        .unwrap();
    catalog
        .append_feedback_event(&exported_event("last", LearningScope::Global, photo))
        .unwrap();
    catalog.discard_recipe_history(photo).unwrap();
    let first = catalog
        .learning_evidence_page(&LearningScope::Global, 0, 1)
        .unwrap();
    assert!(first.has_more);
    assert_eq!(first.unavailable_edit_event_ids, ["approved-no-change"]);
    let second = catalog
        .learning_evidence_page(&LearningScope::Global, first.next_after_sequence, 1)
        .unwrap();
    assert!(!second.has_more);
    assert_eq!(second.next_after_sequence, 3);
    assert_eq!(second.report.excluded[0].event_id, "last");
    catalog
        .append_feedback_forget_fact(&NewFeedbackForgetFact {
            fact_id: "later-forget".into(),
            target_event_id: "approved-no-change".into(),
            occurred_at_unix_ms: 2,
            reason: None,
        })
        .unwrap();
    let replay = catalog
        .learning_evidence_page(&LearningScope::Global, 0, 1)
        .unwrap();
    assert!(replay.report.approved_edit_references.is_empty());
    assert_eq!(replay.report.excluded[0].reason, "forgotten");
    assert!(
        catalog
            .learning_evidence_page(&LearningScope::Global, 0, 0)
            .is_err()
    );
}

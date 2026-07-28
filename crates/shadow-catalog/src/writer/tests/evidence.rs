use std::{
    collections::BTreeSet,
    sync::{Arc, Barrier},
    thread,
};

use shadow_ai::{
    FeedbackAction, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PresentationContext,
};
use shadow_domain::{
    AssetLocation, NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoFlag, Platform,
    RepresentationKind,
};

use crate::{CatalogError, RegisterAsset};

use crate::writer::CatalogActor;

#[test]
fn actor_pages_feedback_and_appends_forget_facts() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/feedback-actor.dng".to_vec(),
                "/photos/feedback-actor.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register asset");
    for event_id in ["actor-event-1", "actor-event-2"] {
        handle
            .append_feedback_event(&NewFeedbackEvent {
                event_id: event_id.into(),
                occurred_at_unix_ms: 1_700_000_001_000,
                scope: LearningScope::Global,
                presentation: PresentationContext {
                    session_id: "actor-session".into(),
                    group_id: None,
                    candidates: vec![],
                    active_model: None,
                },
                action: FeedbackAction::Exported {
                    photo_id: registered.photo_id,
                },
            })
            .expect("append feedback through actor");
    }

    let first_page = handle
        .feedback_events_after(&LearningScope::Global, 0, 1)
        .expect("page feedback through actor");
    assert!(first_page.has_more);
    assert_eq!(first_page.events[0].event_id, "actor-event-1");
    handle
        .append_feedback_forget_fact(&NewFeedbackForgetFact {
            fact_id: "actor-forget-1".into(),
            target_event_id: "actor-event-1".into(),
            occurred_at_unix_ms: 1_700_000_002_000,
            reason: None,
        })
        .expect("append forget through actor");
    assert_eq!(
        handle
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read forgotten through actor"),
        BTreeSet::from(["actor-event-1".into()])
    );
    assert_eq!(
        handle
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("source facts remain")
            .events
            .len(),
        2
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn concurrent_decision_cas_allows_exactly_one_writer_to_advance_the_head() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/concurrent-decision.dng".to_vec(),
                "/photos/concurrent-decision.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register concurrent decision photo");
    let barrier = Arc::new(Barrier::new(2));
    let workers = [
        ("concurrent-pick", PhotoFlag::Picked),
        ("concurrent-reject", PhotoFlag::Rejected),
    ]
    .into_iter()
    .map(|(event_id, after_flag)| {
        let handle = handle.clone();
        let barrier = Arc::clone(&barrier);
        let photo_id = registered.photo_id;
        thread::spawn(move || {
            barrier.wait();
            handle.append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: event_id.into(),
                photo_id,
                occurred_at_unix_ms: 1_700_000_001_000,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence: 0,
                before_flag: PhotoFlag::Unflagged,
                before_rating: 0,
                after_flag,
                after_rating: 0,
            })
        })
    })
    .collect::<Vec<_>>();
    let results = workers
        .into_iter()
        .map(|worker| worker.join().expect("decision worker panicked"))
        .collect::<Vec<_>>();
    assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
    assert_eq!(
        results
            .iter()
            .filter(|result| matches!(result, Err(CatalogError::PhotoDecisionHeadMismatch { .. })))
            .count(),
        1
    );
    let winner = results
        .into_iter()
        .find_map(Result::ok)
        .expect("one winning decision");
    assert_eq!(
        handle
            .photo_decision_state(registered.photo_id)
            .expect("read winning decision"),
        winner.after_state().expect("winning state")
    );
    assert_eq!(
        handle
            .photo_decision_events_after(registered.photo_id, 0, 10)
            .expect("read concurrent decision history")
            .events,
        [winner]
    );
    actor.shutdown().expect("shutdown actor");
}

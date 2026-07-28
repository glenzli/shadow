//! Human photo-decision ledger state, CAS, history, and reopen contracts.

use std::{sync::Arc, thread};

use shadow_ai::LearningScope;
use shadow_domain::{MAX_PHOTO_RATING, PhotoDecisionOrigin};
use uuid::Uuid;

use crate::{
    DesktopSession, ffi, open_desktop_session, tests::fixtures::feedback::test_feedback_session,
};

#[test]
fn review_decision_updates_preserve_complete_state_and_never_write_ai_feedback() {
    let (root, session, left, _) = test_feedback_session();
    let initial = session
        .review_photo_decision_state(&left.photo_id)
        .expect("read initial decision");
    assert_eq!(initial.photo_id, left.photo_id);
    assert_eq!(initial.head_sequence, 0);
    assert_eq!(initial.flag, ffi::FfiDecisionFlag::Unflagged);
    assert_eq!(initial.rating, 0);

    let picked = session
        .set_review_photo_decision(
            &left.photo_id,
            initial.head_sequence,
            ffi::FfiDecisionFlag::Picked,
            initial.rating,
        )
        .expect("pick photo");
    assert_eq!(picked.photo_id, left.photo_id);
    assert_eq!(picked.before_head_sequence, 0);
    assert_eq!(picked.before_flag, ffi::FfiDecisionFlag::Unflagged);
    assert_eq!(picked.before_rating, 0);
    assert_eq!(picked.after_flag, ffi::FfiDecisionFlag::Picked);
    assert_eq!(picked.after_rating, 0);
    assert_eq!(picked.sequence, 1);
    assert_eq!(
        Uuid::parse_str(&picked.event_id).unwrap().get_version_num(),
        7
    );
    assert!(picked.occurred_at_unix_ms > 0);

    let rated = session
        .set_review_photo_decision(&left.photo_id, picked.sequence, picked.after_flag, 4)
        .expect("rate while preserving flag");
    assert_eq!(rated.before_flag, ffi::FfiDecisionFlag::Picked);
    assert_eq!(rated.after_flag, ffi::FfiDecisionFlag::Picked);
    assert_eq!(rated.before_rating, 0);
    assert_eq!(rated.after_rating, 4);

    let rejected = session
        .set_review_photo_decision(
            &left.photo_id,
            rated.sequence,
            ffi::FfiDecisionFlag::Rejected,
            rated.after_rating,
        )
        .expect("reject while preserving rating");
    assert_eq!(rejected.before_rating, 4);
    assert_eq!(rejected.after_rating, 4);
    let current = session
        .review_photo_decision_state(&left.photo_id)
        .expect("read current decision");
    assert_eq!(current.head_sequence, rejected.sequence);
    assert_eq!(current.flag, ffi::FfiDecisionFlag::Rejected);
    assert_eq!(current.rating, 4);

    let events = session
        .catalog
        .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
        .expect("read decision ledger");
    assert_eq!(events.events.len(), 3);
    assert!(
        events
            .events
            .iter()
            .all(|event| event.origin == PhotoDecisionOrigin::Human)
    );
    assert!(
        session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read unrelated AI feedback ledger")
            .events
            .is_empty()
    );
    let review = session.review_page("", "", 10).expect("read Review page");
    let item = review
        .items
        .iter()
        .find(|item| item.photo_id == left.photo_id)
        .expect("updated photo remains in Review page");
    assert_eq!(item.decision_head_sequence, rejected.sequence);
    assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Rejected);
    assert_eq!(item.decision_rating, 4);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove decision fixture");
}

#[test]
fn review_decision_stale_cas_and_invalid_or_noop_requests_append_nothing() {
    let (root, session, left, _) = test_feedback_session();
    let first = session
        .set_review_photo_decision(&left.photo_id, 0, ffi::FfiDecisionFlag::Picked, 0)
        .expect("append first decision");
    assert!(
        session
            .set_review_photo_decision(&left.photo_id, 0, ffi::FfiDecisionFlag::Rejected, 0,)
            .is_err(),
        "stale expected head must lose CAS"
    );
    assert!(
        session
            .set_review_photo_decision(
                &left.photo_id,
                first.sequence,
                first.after_flag,
                first.after_rating,
            )
            .is_err(),
        "no-op state must not become history"
    );
    assert!(
        session
            .set_review_photo_decision(
                &left.photo_id,
                first.sequence,
                first.after_flag,
                MAX_PHOTO_RATING + 1,
            )
            .expect_err("invalid rating must fail before Catalog")
            .to_string()
            .contains("0 through 5")
    );
    let current = session
        .review_photo_decision_state(&left.photo_id)
        .expect("read unchanged decision");
    assert_eq!(current.head_sequence, first.sequence);
    assert_eq!(current.flag, ffi::FfiDecisionFlag::Picked);
    assert_eq!(current.rating, 0);
    let events = session
        .catalog
        .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
        .expect("read unchanged ledger");
    assert_eq!(events.events.len(), 1);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove decision fixture");
}

#[test]
fn review_decision_undo_appends_and_state_survives_reopen() {
    let (root, session, left, _) = test_feedback_session();
    let changed = session
        .set_review_photo_decision(&left.photo_id, 0, ffi::FfiDecisionFlag::Picked, 5)
        .expect("append decision");
    let undone = session
        .set_review_photo_decision(
            &left.photo_id,
            changed.sequence,
            ffi::FfiDecisionFlag::Unflagged,
            0,
        )
        .expect("append inverse decision");
    assert!(undone.sequence > changed.sequence);
    let catalog_path = root.join("catalog.sqlite");
    let cache_path = root.join("cache");
    drop(session);

    let reopened = open_desktop_session(
        catalog_path.to_str().expect("catalog path"),
        cache_path.to_str().expect("cache path"),
    )
    .expect("reopen decision session");
    let state = reopened
        .review_photo_decision_state(&left.photo_id)
        .expect("read reopened decision");
    assert_eq!(state.head_sequence, undone.sequence);
    assert_eq!(state.flag, ffi::FfiDecisionFlag::Unflagged);
    assert_eq!(state.rating, 0);
    let events = reopened
        .catalog
        .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
        .expect("read append-only history");
    assert_eq!(events.events.len(), 2);
    assert_eq!(events.events[0].sequence, changed.sequence);
    assert_eq!(events.events[1].sequence, undone.sequence);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove decision fixture");
}

#[test]
fn concurrent_review_decision_cas_has_exactly_one_winner() {
    let (root, session, left, _) = test_feedback_session();
    let session: Arc<DesktopSession> = Arc::from(session);
    let workers = [ffi::FfiDecisionFlag::Picked, ffi::FfiDecisionFlag::Rejected]
        .into_iter()
        .map(|flag| {
            let session = Arc::clone(&session);
            let photo_id = left.photo_id.clone();
            thread::spawn(move || session.set_review_photo_decision(&photo_id, 0, flag, 0))
        })
        .collect::<Vec<_>>();
    let results = workers
        .into_iter()
        .map(|worker| worker.join().expect("decision worker panicked"))
        .collect::<Vec<_>>();
    assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
    assert_eq!(results.iter().filter(|result| result.is_err()).count(), 1);
    let winner = results
        .into_iter()
        .find_map(Result::ok)
        .expect("one winner");
    let state = session
        .review_photo_decision_state(&left.photo_id)
        .expect("read winning decision");
    assert_eq!(state.head_sequence, winner.sequence);
    assert_eq!(state.flag, winner.after_flag);
    let events = session
        .catalog
        .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
        .expect("read one winning event");
    assert_eq!(events.events.len(), 1);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove decision fixture");
}

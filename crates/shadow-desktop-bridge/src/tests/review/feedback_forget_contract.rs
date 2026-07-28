//! Review feedback eligibility, append-only forgetting, reopen, and concurrency contracts.

use std::{collections::BTreeSet, sync::Arc, thread};

use shadow_ai::{
    FeedbackAction, FeedbackIgnored, LearningScope, NewFeedbackEvent, PresentationContext,
};
use uuid::Uuid;

use crate::{
    DesktopSession, ffi, open_desktop_session,
    tests::fixtures::{
        feedback::{test_feedback_session, training_report},
        review_comparison::ready_review_comparison,
    },
    wall_clock::current_time_ms,
};

#[test]
fn review_forget_accepts_only_active_comparisons_issued_by_the_current_session() {
    let (root, session, left, right) = test_feedback_session();
    let external_event_id = Uuid::now_v7().to_string();
    session
        .catalog
        .append_feedback_event(&NewFeedbackEvent {
            event_id: external_event_id.clone(),
            occurred_at_unix_ms: current_time_ms().expect("current time"),
            scope: LearningScope::Global,
            presentation: PresentationContext {
                session_id: "external-feedback-producer".into(),
                group_id: None,
                candidates: vec![],
                active_model: None,
            },
            action: FeedbackAction::Exported {
                photo_id: left.photo_id.parse().expect("left photo id"),
            },
        })
        .expect("append external Global feedback");
    assert!(
        session
            .forget_review_feedback(&external_event_id)
            .expect_err("external feedback must not enter Review undo")
            .to_string()
            .contains("not an active comparison issued by this Review session")
    );

    let presentation = ready_review_comparison(&session, &left, &right, 3);
    let prior_session_receipt = session
        .record_review_comparison(
            &presentation.presentation_id,
            ffi::FfiPairwiseOutcome::KeepBoth,
        )
        .expect("record current-session comparison");
    let catalog_path = root.join("catalog.sqlite");
    let cache_path = root.join("cache");
    drop(session);

    let reopened = open_desktop_session(
        catalog_path.to_str().expect("catalog path"),
        cache_path.to_str().expect("cache path"),
    )
    .expect("reopen feedback session");
    assert!(
        reopened
            .forget_review_feedback(&prior_session_receipt.event_id)
            .expect_err("an earlier session's comparison must not enter Review undo")
            .to_string()
            .contains("not an active comparison issued by this Review session")
    );
    assert!(
        reopened
            .catalog
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read unchanged forget set")
            .is_empty()
    );

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn review_feedback_reopens_forgets_append_only_and_never_trains_without_features() {
    let (root, session, left, right) = test_feedback_session();
    let presentation = ready_review_comparison(&session, &left, &right, 4);
    let receipt = session
        .record_review_comparison(
            &presentation.presentation_id,
            ffi::FfiPairwiseOutcome::LeftPreferred,
        )
        .expect("record comparison");
    let page = session
        .catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .expect("read comparison");
    let no_feature_report = training_report(&page.events, BTreeSet::new());
    assert!(no_feature_report.batch.examples.is_empty());
    assert_eq!(
        no_feature_report.ignored,
        [FeedbackIgnored::MissingFrozenFeature {
            event_id: receipt.event_id.clone(),
            photo_id: left.photo_id.parse().unwrap(),
        }]
    );

    let forgotten = session
        .forget_review_feedback(&receipt.event_id)
        .expect("append forget fact");
    assert_eq!(forgotten.target_event_id, receipt.event_id);
    assert_eq!(forgotten.sequence, 1);
    assert_eq!(
        Uuid::parse_str(&forgotten.fact_id)
            .unwrap()
            .get_version_num(),
        7
    );
    assert!(forgotten.occurred_at_unix_ms > 0);
    let forgotten_ids = session
        .catalog
        .forgotten_feedback_event_ids(&LearningScope::Global)
        .expect("read forgotten ids");
    assert_eq!(forgotten_ids, BTreeSet::from([receipt.event_id.clone()]));
    let forgotten_report = training_report(&page.events, forgotten_ids);
    assert!(forgotten_report.batch.examples.is_empty());
    assert_eq!(
        forgotten_report.ignored,
        [FeedbackIgnored::Forgotten {
            event_id: receipt.event_id.clone(),
        }]
    );
    assert!(
        session
            .forget_review_feedback(&receipt.event_id)
            .expect_err("duplicate forget must fail")
            .to_string()
            .contains("not an active comparison issued by this Review session")
    );

    let catalog_path = root.join("catalog.sqlite");
    let cache_path = root.join("cache");
    drop(session);
    let reopened = open_desktop_session(
        catalog_path.to_str().expect("catalog path"),
        cache_path.to_str().expect("cache path"),
    )
    .expect("reopen feedback session");
    let reopened_page = reopened
        .catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .expect("read event after reopen");
    assert_eq!(reopened_page.events.len(), 1);
    assert_eq!(reopened_page.events[0].event_id, receipt.event_id);
    assert_eq!(
        reopened
            .catalog
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read forget fact after reopen"),
        BTreeSet::from([receipt.event_id.clone()])
    );
    assert!(
        reopened
            .forget_review_feedback(&receipt.event_id)
            .expect_err("a reopened session must not forget an earlier session's event")
            .to_string()
            .contains("not an active comparison issued by this Review session")
    );
    assert!(
        reopened
            .forget_review_feedback(&Uuid::now_v7().to_string())
            .expect_err("unknown event must fail")
            .to_string()
            .contains("not an active comparison issued by this Review session")
    );
    assert!(
        reopened
            .forget_review_feedback("not-a-uuid")
            .expect_err("malformed event id must fail")
            .to_string()
            .contains("parse Review feedback event id")
    );

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn concurrent_review_feedback_consumes_one_presentation_once_and_single_forget_fact() {
    let (root, session, left, right) = test_feedback_session();
    let presentation = ready_review_comparison(&session, &left, &right, 5);
    let session: Arc<DesktopSession> = Arc::from(session);
    let record_workers = (0..8)
        .map(|_| {
            let session = Arc::clone(&session);
            let presentation_id = presentation.presentation_id.clone();
            thread::spawn(move || {
                session
                    .record_review_comparison(&presentation_id, ffi::FfiPairwiseOutcome::KeepBoth)
            })
        })
        .collect::<Vec<_>>();
    let results = record_workers
        .into_iter()
        .map(|worker| worker.join().expect("record worker panicked"))
        .collect::<Vec<_>>();
    assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
    assert_eq!(
        results
            .iter()
            .filter_map(|result| result.as_ref().err())
            .filter(|error| error.to_string().contains("unknown or expired"))
            .count(),
        7
    );
    let target = results
        .into_iter()
        .find_map(Result::ok)
        .expect("one record receipt")
        .event_id;
    let forget_workers = (0..4)
        .map(|_| {
            let session = Arc::clone(&session);
            let target = target.clone();
            thread::spawn(move || session.forget_review_feedback(&target))
        })
        .collect::<Vec<_>>();
    let forget_results = forget_workers
        .into_iter()
        .map(|worker| worker.join().expect("forget worker panicked"))
        .collect::<Vec<_>>();
    assert_eq!(
        forget_results
            .iter()
            .filter(|result| result.is_ok())
            .count(),
        1
    );
    assert_eq!(
        forget_results
            .iter()
            .filter_map(|result| result.as_ref().err())
            .filter(|error| {
                error
                    .to_string()
                    .contains("not an active comparison issued by this Review session")
            })
            .count(),
        3
    );
    assert_eq!(
        session
            .catalog
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read concurrent forget result"),
        BTreeSet::from([target])
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

//! Review decisions, comparisons, evidence, and feedback contracts.

use super::*;

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

#[test]
#[allow(clippy::too_many_lines)]
fn review_comparison_maps_and_persists_all_five_explicit_outcomes() {
    let (root, session, left, right) = test_feedback_session();
    let cases = [
        (
            ffi::FfiPairwiseOutcome::LeftPreferred,
            PairwiseOutcome::LeftPreferred,
        ),
        (
            ffi::FfiPairwiseOutcome::RightPreferred,
            PairwiseOutcome::RightPreferred,
        ),
        (ffi::FfiPairwiseOutcome::KeepBoth, PairwiseOutcome::KeepBoth),
        (
            ffi::FfiPairwiseOutcome::KeepNeither,
            PairwiseOutcome::KeepNeither,
        ),
        (
            ffi::FfiPairwiseOutcome::CannotCompare,
            PairwiseOutcome::CannotCompare,
        ),
    ];
    let mut receipts = Vec::new();
    for (index, (outcome, _)) in cases.iter().enumerate() {
        let presentation =
            ready_review_comparison(&session, &left, &right, u8::try_from(index + 1).unwrap());
        receipts.push(
            session
                .record_review_comparison(&presentation.presentation_id, *outcome)
                .expect("record Review comparison"),
        );
    }

    let page = session
        .catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .expect("read persisted Review comparisons");
    assert_eq!(page.events.len(), cases.len());
    assert!(!page.has_more);
    for (index, ((_, expected_outcome), event)) in cases.iter().zip(&page.events).enumerate() {
        let receipt = &receipts[index];
        assert_eq!(receipt.event_id, event.event_id);
        assert_eq!(receipt.sequence, event.sequence);
        assert_eq!(receipt.occurred_at_unix_ms, event.occurred_at_unix_ms);
        assert_eq!(receipt.sequence, u64::try_from(index + 1).unwrap());
        assert_eq!(
            Uuid::parse_str(&receipt.event_id)
                .unwrap()
                .get_version_num(),
            7
        );
        assert!(receipt.occurred_at_unix_ms > 0);
        assert_eq!(event.scope, LearningScope::Global);
        assert_eq!(
            event.presentation.session_id,
            session.review.feedback_session_id()
        );
        assert!(event.presentation.group_id.is_none());
        assert!(event.presentation.active_model.is_none());
        assert_eq!(event.presentation.candidates.len(), 2);
        assert_eq!(event.presentation.candidates[0].position, 0);
        assert_eq!(event.presentation.candidates[1].position, 1);
        assert_eq!(
            event.presentation.candidates[0].visible_fraction,
            AiUnitInterval::ONE
        );
        assert_eq!(
            event.presentation.candidates[1].visible_fraction,
            AiUnitInterval::ONE
        );
        assert!(!event.presentation.candidates[0].inspected_at_one_to_one);
        assert!(!event.presentation.candidates[1].inspected_at_one_to_one);
        assert!(event.presentation.candidates[0].feature.is_none());
        assert!(event.presentation.candidates[1].feature.is_none());
        let left_visual = event.presentation.candidates[0]
            .visual
            .as_ref()
            .expect("left visual provenance");
        let right_visual = event.presentation.candidates[1]
            .visual
            .as_ref()
            .expect("right visual provenance");
        assert_eq!(
            left_visual.artifact.representation_id.to_string(),
            left.representation_id
        );
        assert_eq!(
            right_visual.artifact.representation_id.to_string(),
            right.representation_id
        );
        assert_eq!(
            left_visual.artifact.blob_digest_hex,
            encode_hex(&left.record.artifact.blob_digest)
        );
        assert_eq!(
            right_visual.artifact.blob_digest_hex,
            encode_hex(&right.record.artifact.blob_digest)
        );
        assert_eq!(left_visual.frame.surface_id, REVIEW_COMPARE_SURFACE_ID);
        assert_eq!(
            left_visual.frame.surface_revision,
            REVIEW_COMPARE_SURFACE_REVISION
        );
        assert_eq!(
            left_visual.frame.fit_mode,
            PresentedFitMode::PreserveAspectFit
        );
        assert_eq!(left_visual.frame.decoder_id, REVIEW_COMPARE_DECODER_ID);
        assert_eq!(left_visual.frame.pixel_format, REVIEW_COMPARE_PIXEL_FORMAT);
        assert_eq!(
            left_visual.frame.pixel_hash_algorithm,
            REVIEW_COMPARE_PIXEL_HASH_ALGORITHM
        );
        assert!(matches!(
            event.action,
            FeedbackAction::PairwiseComparison {
                left: event_left,
                right: event_right,
                outcome,
            } if event_left.to_string() == left.photo_id
                && event_right.to_string() == right.photo_id
                && outcome == *expected_outcome
        ));
    }

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn review_handles_reject_forgery_cross_session_and_same_photo() {
    let (root, session, left, right) = test_feedback_session();
    assert!(
        session
            .prepare_review_comparison(&left.visual_handle, &left.visual_handle)
            .expect_err("same photo must fail")
            .to_string()
            .contains("two different photos")
    );
    assert!(
        session
            .prepare_review_comparison("not-a-handle", &right.visual_handle)
            .expect_err("plain identifiers must not be accepted")
            .to_string()
            .contains("invalid Review grid visual handle prefix")
    );
    let mut forged = left.visual_handle.clone();
    let replacement = if forged.ends_with('0') { '1' } else { '0' };
    forged.pop();
    forged.push(replacement);
    assert!(
        session
            .prepare_review_comparison(&forged, &right.visual_handle)
            .expect_err("forged handle must fail")
            .to_string()
            .contains("signature is invalid")
    );

    let other_root = root.join("other-session");
    let other = open_desktop_session(
        other_root.join("catalog.sqlite").to_str().unwrap(),
        root.join("cache").to_str().unwrap(),
    )
    .expect("open second session");
    assert!(
        other
            .load_review_visual(&left.visual_handle)
            .expect_err("grid handles are session-bound")
            .to_string()
            .contains("signature is invalid for this session")
    );
    assert!(
        session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read empty feedback page")
            .events
            .is_empty()
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
#[allow(clippy::too_many_lines)]
fn review_comparison_requires_verified_frames_confirmation_and_consumes_once() {
    let (root, session, left, right) = test_feedback_session();
    let presentation = session
        .prepare_review_comparison(&left.visual_handle, &right.visual_handle)
        .expect("prepare comparison");
    assert!(
        session
            .record_review_visual_frame(
                &presentation.left_request_ticket,
                "qt-test-1",
                800,
                600,
                4,
                3,
                &"11".repeat(32),
            )
            .expect_err("receipt before load must fail")
            .to_string()
            .contains("bytes must load successfully")
    );
    assert!(
        session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::LeftPreferred,
            )
            .expect_err("unconfirmed comparison must fail")
            .to_string()
            .contains("confirmed ready")
    );

    let left_payload = session
        .load_review_visual(&presentation.left_request_ticket)
        .expect("load exact left comparison bytes");
    assert!(left_payload.requires_frame_receipt);
    assert_eq!(left_payload.bytes, left.bytes);
    record_test_frame(&session, &presentation.left_request_ticket, 1);
    record_test_frame(&session, &presentation.left_request_ticket, 1);
    assert!(
        session
            .record_review_visual_frame(
                &presentation.left_request_ticket,
                "qt-test-1",
                800,
                600,
                4,
                3,
                &"22".repeat(32),
            )
            .expect_err("different duplicate frame receipt must fail")
            .to_string()
            .contains("different frame receipt")
    );
    assert!(
        session
            .confirm_review_comparison_ready(
                &presentation.presentation_id,
                &presentation.left_request_ticket,
                &presentation.right_request_ticket,
            )
            .expect_err("missing right frame must fail")
            .to_string()
            .contains("right Review comparison visual is not fully presented")
    );
    let right_payload = session
        .load_review_visual(&presentation.right_request_ticket)
        .expect("load exact right comparison bytes");
    assert!(right_payload.requires_frame_receipt);
    assert_eq!(right_payload.bytes, right.bytes);
    record_test_frame(&session, &presentation.right_request_ticket, 2);
    assert!(
        session
            .confirm_review_comparison_ready(
                &presentation.presentation_id,
                &presentation.right_request_ticket,
                &presentation.left_request_ticket,
            )
            .expect_err("swapped tickets must fail")
            .to_string()
            .contains("do not belong")
    );
    session
        .confirm_review_comparison_ready(
            &presentation.presentation_id,
            &presentation.left_request_ticket,
            &presentation.right_request_ticket,
        )
        .expect("confirm ready");
    session
        .record_review_comparison(
            &presentation.presentation_id,
            ffi::FfiPairwiseOutcome::LeftPreferred,
        )
        .expect("record once");
    assert!(
        session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::LeftPreferred,
            )
            .expect_err("consumed presentation must fail")
            .to_string()
            .contains("unknown or expired")
    );
    assert!(
        session
            .load_review_visual(&presentation.left_request_ticket)
            .expect_err("consumed request ticket must fail")
            .to_string()
            .contains("unknown or expired")
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn cancel_review_comparison_expires_both_request_tickets_without_feedback() {
    let (root, session, left, right) = test_feedback_session();
    let presentation = session
        .prepare_review_comparison(&left.visual_handle, &right.visual_handle)
        .expect("prepare comparison to cancel");
    session
        .cancel_review_comparison(&presentation.presentation_id)
        .expect("cancel comparison");
    for ticket in [
        &presentation.left_request_ticket,
        &presentation.right_request_ticket,
    ] {
        assert!(
            session
                .load_review_visual(ticket)
                .expect_err("canceled ticket must expire")
                .to_string()
                .contains("unknown or expired")
        );
    }
    assert!(
        session
            .cancel_review_comparison(&presentation.presentation_id)
            .expect_err("cancel is single-use")
            .to_string()
            .contains("unknown or expired")
    );
    assert!(
        session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read empty feedback")
            .events
            .is_empty()
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn catalog_failure_retains_ready_presentation_for_retry_or_cancel() {
    let (root, session, left, right) = test_feedback_session();
    // This state cannot be produced by the public Review page, but it is a
    // stable failure injection: the session signs a visual record owned by
    // another photo and Catalog remains the authoritative ownership gate.
    let mismatched_left_handle = session
        .review
        .encode_grid_visual_handle(&ReviewVisualSelection {
            photo_id: left.photo_id.parse().expect("left photo id"),
            record: right.record.clone(),
        })
        .expect("sign deliberately mismatched fixture handle");
    let presentation = session
        .prepare_review_comparison(&mismatched_left_handle, &right.visual_handle)
        .expect("prepare ownership failure fixture");
    session
        .load_review_visual(&presentation.left_request_ticket)
        .expect("load mismatched left bytes");
    session
        .load_review_visual(&presentation.right_request_ticket)
        .expect("load right bytes");
    record_test_frame(&session, &presentation.left_request_ticket, 8);
    record_test_frame(&session, &presentation.right_request_ticket, 9);
    session
        .confirm_review_comparison_ready(
            &presentation.presentation_id,
            &presentation.left_request_ticket,
            &presentation.right_request_ticket,
        )
        .expect("confirm ownership failure fixture");

    for _ in 0..2 {
        let error = session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::LeftPreferred,
            )
            .expect_err("Catalog ownership failure must retain presentation")
            .to_string();
        assert!(
            error.contains("is not owned by candidate photo"),
            "unexpected Catalog ownership error: {error}"
        );
    }
    session
        .cancel_review_comparison(&presentation.presentation_id)
        .expect("retained failed presentation remains cancelable");
    assert!(
        session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read empty feedback after Catalog failure")
            .events
            .is_empty()
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn exact_grid_selection_survives_preferred_artifact_replacement() {
    let (root, session, left, right) = test_feedback_session();
    let old_digest = left.record.artifact.blob_digest;
    let replacement = replace_feedback_visual(&session, &left, 9);
    assert_ne!(replacement.artifact.blob_digest, old_digest);
    assert_eq!(
        session
            .catalog
            .preferred_cached_artifact(left.record.representation_id)
            .expect("read replacement")
            .expect("preferred replacement")
            .artifact
            .blob_digest,
        replacement.artifact.blob_digest
    );

    let grid_payload = session
        .load_review_visual(&left.visual_handle)
        .expect("load old exact grid artifact");
    assert!(!grid_payload.requires_frame_receipt);
    assert_eq!(grid_payload.bytes, left.bytes);
    let presentation = ready_review_comparison(&session, &left, &right, 7);
    session
        .record_review_comparison(
            &presentation.presentation_id,
            ffi::FfiPairwiseOutcome::LeftPreferred,
        )
        .expect("record exact old presentation");
    let page = session
        .catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .expect("read exact event");
    assert_eq!(
        page.events[0].presentation.candidates[0]
            .visual
            .as_ref()
            .expect("left provenance")
            .artifact
            .blob_digest_hex,
        encode_hex(&old_digest)
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

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

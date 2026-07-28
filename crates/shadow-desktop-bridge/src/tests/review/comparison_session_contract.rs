//! Review comparison admission, visual evidence, terminal, and persistence contracts.

use shadow_ai::{
    FeedbackAction, LearningScope, PairwiseOutcome, PresentedFitMode,
    UnitInterval as AiUnitInterval,
};
use uuid::Uuid;

use crate::{
    digest_hex::encode_hex,
    ffi, open_desktop_session,
    review_service::{
        REVIEW_COMPARE_DECODER_ID, REVIEW_COMPARE_PIXEL_FORMAT,
        REVIEW_COMPARE_PIXEL_HASH_ALGORITHM, REVIEW_COMPARE_SURFACE_ID,
        REVIEW_COMPARE_SURFACE_REVISION, ReviewVisualSelection,
    },
    tests::fixtures::{
        feedback::test_feedback_session,
        review_comparison::{ready_review_comparison, record_test_frame},
    },
};

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

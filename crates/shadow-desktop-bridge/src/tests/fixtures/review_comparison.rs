//! Review comparison readiness and frame-receipt fixtures.

use crate::{DesktopSession, ffi};

use super::feedback::TestFeedbackCandidate;

pub(in crate::tests) fn record_test_frame(
    session: &DesktopSession,
    request_ticket: &str,
    byte: u8,
) {
    session
        .record_review_visual_frame(
            request_ticket,
            "qt-test-1",
            800,
            600,
            4,
            3,
            &format!("{byte:02x}").repeat(32),
        )
        .expect("record fixture frame receipt");
}

pub(in crate::tests) fn ready_review_comparison(
    session: &DesktopSession,
    left: &TestFeedbackCandidate,
    right: &TestFeedbackCandidate,
    frame_seed: u8,
) -> ffi::FfiReviewComparisonPresentation {
    let presentation = session
        .prepare_review_comparison(&left.visual_handle, &right.visual_handle)
        .expect("prepare fixture comparison");
    let left_payload = session
        .load_review_visual(&presentation.left_request_ticket)
        .expect("load fixture left visual");
    let right_payload = session
        .load_review_visual(&presentation.right_request_ticket)
        .expect("load fixture right visual");
    assert_eq!(left_payload.bytes, left.bytes);
    assert_eq!(right_payload.bytes, right.bytes);
    assert!(left_payload.requires_frame_receipt);
    assert!(right_payload.requires_frame_receipt);
    record_test_frame(session, &presentation.left_request_ticket, frame_seed);
    record_test_frame(
        session,
        &presentation.right_request_ticket,
        frame_seed.wrapping_add(1),
    );
    session
        .confirm_review_comparison_ready(
            &presentation.presentation_id,
            &presentation.left_request_ticket,
            &presentation.right_request_ticket,
        )
        .expect("confirm fixture comparison");
    presentation
}

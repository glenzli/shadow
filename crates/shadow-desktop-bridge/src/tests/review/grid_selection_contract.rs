//! Exact Review-grid visual identity and replacement-stability contracts.

use shadow_ai::LearningScope;

use crate::{
    digest_hex::encode_hex,
    ffi,
    tests::fixtures::{
        feedback::{replace_feedback_visual, test_feedback_session},
        review_comparison::ready_review_comparison,
    },
};

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

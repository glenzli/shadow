//! Selected-preview refresh preserves immutable originals and exact cache CAS.

use crate::tests::fixtures::feedback::{replace_feedback_visual, test_feedback_session};

#[test]
fn selected_preview_refresh_invalidates_only_the_current_signed_visual() {
    let (root, session, left, right) = test_feedback_session();
    assert_eq!(
        session
            .refresh_selected_review_previews(vec![left.visual_handle.clone()])
            .expect("invalidate selected preview"),
        1
    );
    assert!(
        session
            .catalog
            .preferred_cached_artifact(left.record.representation_id)
            .expect("read invalidated selection")
            .is_none()
    );
    assert!(
        session
            .catalog
            .preferred_cached_artifact(right.record.representation_id)
            .expect("read unselected visual")
            .is_some(),
        "refreshing one selected preview must never affect another photo"
    );
    assert_eq!(
        session
            .refresh_selected_review_previews(vec![left.visual_handle])
            .expect("repeat exact invalidation"),
        0,
        "a stale selection must not remove a later replacement"
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn selected_preview_refresh_never_removes_a_replacement_written_after_selection() {
    let (root, session, left, _) = test_feedback_session();
    let replacement = replace_feedback_visual(&session, &left, 73);
    assert_eq!(
        session
            .refresh_selected_review_previews(vec![left.visual_handle])
            .expect("ignore stale selection"),
        0
    );
    assert_eq!(
        session
            .catalog
            .preferred_cached_artifact(replacement.representation_id)
            .expect("read replacement")
            .expect("replacement remains current")
            .artifact
            .blob_digest,
        replacement.artifact.blob_digest
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn selected_preview_refresh_rejects_session_only_embedded_visuals() {
    let (root, session, left, _) = test_feedback_session();
    let error = session
        .refresh_selected_review_previews(vec![
            left.visual_handle.clone(),
            "shadow-grid-session-v1.expired".to_owned(),
        ])
        .expect_err("embedded previews have no durable artifact to refresh");
    assert!(
        error
            .to_string()
            .contains("requires durable generated proxies")
    );
    assert!(
        session
            .catalog
            .preferred_cached_artifact(left.record.representation_id)
            .expect("read durable preview after rejected mixed selection")
            .is_some(),
        "a rejected mixed selection must not partially invalidate durable previews"
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

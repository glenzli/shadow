//! Preview cancellation ownership, thread-safety, and admission bounds.

use std::path::Path;

use crate::{
    BridgeError, EditPreviewCancellation, LibRawEditPreviewSession, MAX_WARM_EDIT_PREVIEW_EDGE,
    PhotoEditPreviewSession,
};

#[test]
fn preview_cancellation_is_shared_one_shot_state() {
    let cancellation =
        EditPreviewCancellation::new().expect("allocate native preview cancellation");
    let clone = cancellation.clone();
    assert!(
        clone.cancel(),
        "first request_stop wins across SharedPtr clones"
    );
    assert!(
        !cancellation.cancel(),
        "later cancellation requests are idempotent"
    );
}

#[test]
fn warm_edit_session_is_send_sync_and_bounded_before_raw_io() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<LibRawEditPreviewSession>();
    assert_send_sync::<PhotoEditPreviewSession>();

    assert_eq!(
        std::any::TypeId::of::<PhotoEditPreviewSession>(),
        std::any::TypeId::of::<LibRawEditPreviewSession>(),
        "the source-neutral API must not add a second session allocation or threading model"
    );

    for max_edge in [0, MAX_WARM_EDIT_PREVIEW_EDGE + 1] {
        let error = PhotoEditPreviewSession::open(
            Path::new("fixture-that-must-not-be-opened.raw"),
            max_edge,
        )
        .expect_err("invalid warm bound must fail before opening the source");
        assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
    }
}

use super::{library_cursor, recipe_cursor};
use crate::ffi;

#[test]
fn empty_history_cursor_is_the_first_page_sentinel() {
    let cursor = ffi::FfiHistoryCursor::default();
    assert!(recipe_cursor(&cursor).expect("Recipe cursor").is_none());
    assert!(library_cursor(&cursor).expect("Library cursor").is_none());
}

#[test]
fn incomplete_history_cursor_is_rejected() {
    let cursor = ffi::FfiHistoryCursor {
        created_at_ms: 42,
        commit_id: String::new(),
    };
    assert!(recipe_cursor(&cursor).is_err());
    assert!(library_cursor(&cursor).is_err());
}

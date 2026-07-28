//! Facade-local policy, title, and cursor invariants.

use shadow_domain::{EntityId, RepresentationId};

use crate::review_service::{file_name, parse_cursor};

#[test]
fn display_title_uses_the_final_path_component() {
    assert_eq!(file_name("/photos/trip/input.dng"), "input.dng");
    assert_eq!(file_name("input.dng"), "input.dng");
}

#[test]
fn partial_review_cursor_is_rejected() {
    assert!(parse_cursor("/photos/a.dng", "").is_err());
    assert!(parse_cursor("", &RepresentationId::new_v7().to_string()).is_err());
}

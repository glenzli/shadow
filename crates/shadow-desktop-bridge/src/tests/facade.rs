//! Facade-local policy, title, and cursor invariants.

use super::*;

#[test]
fn edit_preview_policy_controls_analysis_recipe_and_cache_admission() {
    let interactive = EditPreviewPolicy::from_ffi(ffi::FfiEditPreviewPolicy::Interactive)
        .expect("interactive policy");
    assert!(interactive.uses_working_recipe());
    assert!(!interactive.requires_analysis());
    assert!(!interactive.admits_durable_cache());
    assert!(!interactive.returns_sensor_diagnostics());

    let settled =
        EditPreviewPolicy::from_ffi(ffi::FfiEditPreviewPolicy::Settled).expect("settled policy");
    assert!(settled.uses_working_recipe());
    assert!(settled.requires_analysis());
    assert!(settled.admits_durable_cache());
    assert!(settled.returns_sensor_diagnostics());

    let neutral = EditPreviewPolicy::from_ffi(ffi::FfiEditPreviewPolicy::NeutralBefore)
        .expect("neutral policy");
    assert!(!neutral.uses_working_recipe());
    assert!(neutral.requires_analysis());
    assert!(!neutral.admits_durable_cache());
    assert!(neutral.returns_sensor_diagnostics());

    assert!(admits_recipe_preview_cache(
        settled,
        PreviewTerminalClaim::Completed
    ));
    assert!(!admits_recipe_preview_cache(
        settled,
        PreviewTerminalClaim::Cancelled
    ));
    assert!(!admits_recipe_preview_cache(
        interactive,
        PreviewTerminalClaim::Completed
    ));
}

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

use shadow_domain::{EntityId, RepresentationId};

use super::*;

#[test]
fn policy_controls_analysis_recipe_and_cache_admission() {
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
fn public_preview_entries_preserve_unique_terminal_ownership() {
    let root = std::env::temp_dir().join(format!(
        "shadow-edit-preview-service-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    let session = crate::open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open desktop session");

    let cancelled = session.begin_basic_edit_preview();
    assert_ne!(cancelled, 0);
    assert!(session.cancel_basic_edit_preview(cancelled));
    assert_eq!(
        session
            .claim_basic_edit_preview_terminal(cancelled)
            .expect("claim cancelled terminal"),
        ffi::FfiEditPreviewTerminal::Cancelled
    );
    assert!(
        session
            .claim_basic_edit_preview_terminal(cancelled)
            .is_err()
    );

    let completed = session.begin_basic_edit_preview();
    assert_ne!(completed, 0);
    assert_eq!(
        session
            .claim_basic_edit_preview_terminal(completed)
            .expect("claim completed terminal"),
        ffi::FfiEditPreviewTerminal::Completed
    );
    assert!(!session.cancel_basic_edit_preview(completed));

    drop(session);
    std::fs::remove_dir_all(root).expect("remove preview-service fixture");
}

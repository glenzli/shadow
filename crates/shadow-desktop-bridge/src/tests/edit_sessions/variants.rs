//! Photo Variant switching and stale-editor isolation contracts.

use crate::tests::fixtures::{
    edit_session::test_edit_session,
    grade_stack::{assert_close, ffi_parameters},
};

#[test]
fn variants_restore_independent_working_recipes_and_reject_stale_writers() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let original = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("load original Variant");
    assert_eq!(original.variants.len(), 1);
    assert_eq!(original.active_variant_id, original.variants[0].variant_id);
    assert!(original.variants[0].is_default);

    let original = session
        .autosave_basic_edit_working_for_variant_at(
            &photo_id,
            &source_path,
            "",
            "",
            Some(&original.active_variant_id),
            &ffi_parameters(0.2, 1.0, [0.0; 2], 1.0),
            1_000,
        )
        .expect("save original Variant");
    let original_variant_id = original.active_variant_id.clone();
    let original_commit_id = original.working_commit_id.clone();

    let alternate = session
        .create_photo_variant(&photo_id, &source_path, "High contrast")
        .expect("create alternate Variant");
    assert_eq!(alternate.variants.len(), 2);
    assert_ne!(alternate.active_variant_id, original_variant_id);
    assert_eq!(alternate.working_commit_id, original_commit_id);
    let alternate_variant_id = alternate.active_variant_id.clone();

    let alternate = session
        .autosave_basic_edit_working_for_variant_at(
            &photo_id,
            &source_path,
            &alternate.working_commit_id,
            &alternate.working_commit_id,
            Some(&alternate_variant_id),
            &ffi_parameters(0.9, 1.2, [0.03, -0.02], 0.94),
            2_000,
        )
        .expect("save alternate Variant");
    assert_close(alternate.settings.grade_nodes[0].basic.exposure_stops, 0.9);

    let original = session
        .activate_photo_variant(&photo_id, &source_path, &original_variant_id)
        .expect("restore original Variant");
    assert_eq!(original.working_commit_id, original_commit_id);
    assert_close(original.settings.grade_nodes[0].basic.exposure_stops, 0.2);

    let error = session
        .autosave_basic_edit_working_for_variant_at(
            &photo_id,
            &source_path,
            &original.working_commit_id,
            &original.working_commit_id,
            Some(&alternate_variant_id),
            &ffi_parameters(1.7, 1.0, [0.0; 2], 1.0),
            3_000,
        )
        .expect_err("stale alternate editor must not write into original Variant");
    assert!(
        error.to_string().contains("active Variant"),
        "unexpected stale Variant error: {error:#}"
    );

    let still_original = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("reload original after rejected stale write");
    assert_eq!(still_original.working_commit_id, original_commit_id);
    assert_close(
        still_original.settings.grade_nodes[0].basic.exposure_stops,
        0.2,
    );

    let alternate = session
        .activate_photo_variant(&photo_id, &source_path, &alternate_variant_id)
        .expect("restore alternate Variant");
    assert_close(alternate.settings.grade_nodes[0].basic.exposure_stops, 0.9);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove Variant fixture");
}

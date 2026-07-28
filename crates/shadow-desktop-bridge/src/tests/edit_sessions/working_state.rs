//! Working-recipe autosave, conflict recovery, and compatibility contracts.

use rusqlite::{Connection, params};
use shadow_catalog::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget};
use shadow_domain::{
    CURRENT_RECIPE_SCHEMA_VERSION, EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId,
    RecipeSnapshot,
};

use crate::{
    open_desktop_session,
    recipe_v1::{GradeStackDraft, grade_stack_recipe_v1_snapshot},
    session_edit_history::{LIBRARY_EDIT_MAIN_REF, NAMED_VERSION_REF_PREFIX, WORKING_RECIPE_REF},
    tests::fixtures::{
        edit_session::test_edit_session,
        grade_stack::{assert_close, ffi_parameters},
    },
};

#[test]
fn autosave_advances_working_without_creating_a_named_version() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let named = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &ffi_parameters(0.1, 1.05, [0.0; 2], 1.0),
            "Baseline",
            1_000,
        )
        .expect("save named baseline");
    let named_id = named.working_commit_id;
    let library_head_before = session
        .catalog
        .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
        .expect("read Library head")
        .expect("named save creates Library head")
        .commit_id;

    let autosaved = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            &named_id,
            &named_id,
            &ffi_parameters(0.8, 1.2, [0.03, -0.02], 0.92),
            1_500,
        )
        .expect("autosave working recipe");
    assert_ne!(autosaved.working_commit_id, named_id);
    assert_eq!(autosaved.versions.len(), 1);
    assert_eq!(autosaved.versions[0].commit_id, named_id);
    assert!(!autosaved.versions[0].is_working);
    assert_close(autosaved.settings.grade_nodes[0].basic.exposure_stops, 0.8);

    let parsed_photo_id: PhotoId = photo_id.parse().expect("parse photo id");
    let commits = session
        .catalog
        .recipe_commits(parsed_photo_id)
        .expect("list immutable commits");
    assert_eq!(commits.len(), 2);
    assert!(
        session
            .catalog
            .recipe_ref(
                parsed_photo_id,
                &format!("{NAMED_VERSION_REF_PREFIX}{}", autosaved.working_commit_id)
            )
            .expect("read autosave version ref")
            .is_none()
    );
    assert_eq!(
        session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read Library head after autosave")
            .expect("Library head remains")
            .commit_id,
        library_head_before
    );

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen desktop session");
    let restored = reopened
        .photo_edit_state(&photo_id, &source_path)
        .expect("restore working autosave");
    assert_eq!(restored.working_commit_id, autosaved.working_commit_id);
    assert_eq!(restored.versions.len(), 1);
    assert_close(restored.settings.grade_nodes[0].basic.exposure_stops, 0.8);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove autosave fixture");
}

#[test]
fn autosave_persists_a_non_neutral_oklab_lightness_curve() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let mut settings = ffi_parameters(0.2, 1.0, [0.0; 2], 1.0);
    settings.grade_nodes[0].fine.oklab_lightness_curve_points =
        vec![0.0, 0.0, 0.42, 0.61, 1.0, 1.0];

    let autosaved = session
        .autosave_basic_edit_working_at(&photo_id, &source_path, "", "", &settings, 1_500)
        .expect("autosave a non-neutral Oklab lightness curve");
    assert_eq!(
        autosaved.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        settings.grade_nodes[0].fine.oklab_lightness_curve_points
    );

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen autosave fixture");
    let restored = reopened
        .photo_edit_state(&photo_id, &source_path)
        .expect("restore Oklab lightness autosave");
    assert_eq!(
        restored.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        settings.grade_nodes[0].fine.oklab_lightness_curve_points
    );

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove Oklab autosave fixture");
}

#[test]
fn autosave_persists_combined_perceptual_color_controls() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let mut settings = ffi_parameters(0.2, 1.0, [0.0; 2], 1.0);
    let fine = &mut settings.grade_nodes[0].fine;
    fine.mixer_hue[0] = 0.18;
    fine.mixer_saturation[5] = -0.24;
    fine.mixer_lightness[2] = 0.11;
    fine.color_range_enabled = true;
    fine.color_range_center = 111.0;
    fine.color_range_width = 42.0;
    fine.color_range_softness = 0.64;
    fine.color_range_hue = -14.0;
    fine.color_range_saturation = 0.27;
    fine.color_range_lightness = -0.08;
    fine.selective_color_relative = false;
    fine.selective_color_cmyk[0] = 0.22;
    fine.selective_color_cmyk[19] = -0.17;
    fine.oklab_color_warper_control_points[6] = 0.09;
    fine.oklab_color_warper_control_points[7] = -0.04;
    fine.oklab_color_warper_strength = 0.68;
    fine.oklab_lightness_curve_points = vec![0.0, 0.0, 0.38, 0.49, 0.72, 0.79, 1.0, 1.0];

    session
        .autosave_basic_edit_working_at(&photo_id, &source_path, "", "", &settings, 1_500)
        .expect("autosave combined perceptual color controls");

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen perceptual autosave fixture");
    let restored = reopened
        .photo_edit_state(&photo_id, &source_path)
        .expect("restore combined perceptual autosave");
    let restored_fine = &restored.settings.grade_nodes[0].fine;
    assert_eq!(
        restored_fine.mixer_hue,
        settings.grade_nodes[0].fine.mixer_hue
    );
    assert_eq!(
        restored_fine.mixer_saturation,
        settings.grade_nodes[0].fine.mixer_saturation
    );
    assert_eq!(
        restored_fine.mixer_lightness,
        settings.grade_nodes[0].fine.mixer_lightness
    );
    assert_eq!(
        restored_fine.selective_color_cmyk,
        settings.grade_nodes[0].fine.selective_color_cmyk
    );
    assert_eq!(
        restored_fine.oklab_color_warper_control_points,
        settings.grade_nodes[0]
            .fine
            .oklab_color_warper_control_points
    );
    assert_close(
        restored_fine.oklab_color_warper_strength,
        settings.grade_nodes[0].fine.oklab_color_warper_strength,
    );
    assert_eq!(
        restored_fine.oklab_lightness_curve_points,
        settings.grade_nodes[0].fine.oklab_lightness_curve_points
    );
    assert!(restored_fine.color_range_enabled);
    assert_close(restored_fine.color_range_center, 111.0);
    assert_close(restored_fine.color_range_width, 42.0);
    assert_close(restored_fine.color_range_softness, 0.64);
    assert_close(restored_fine.color_range_hue, -14.0);
    assert_close(restored_fine.color_range_saturation, 0.27);
    assert_close(restored_fine.color_range_lightness, -0.08);
    assert!(!restored_fine.selective_color_relative);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove perceptual autosave fixture");
}

#[test]
fn autosave_recovers_a_stale_missing_working_head_without_losing_the_draft() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            "",
            "",
            &ffi_parameters(0.15, 1.0, [0.0; 2], 1.0),
            1_000,
        )
        .expect("create first working autosave");
    let first_id = first.working_commit_id;

    // Simulate a controller which queued its initial autosave before a different local
    // state task published the first `working` ref. The second call still carries a full
    // current draft, so it must become a child of the discovered working head rather than
    // surfacing a permanent Missing-vs-Some CAS error to the editor.
    let recovered = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            "",
            "",
            &ffi_parameters(0.85, 1.15, [0.02, -0.01], 0.94),
            1_500,
        )
        .expect("rebase stale missing autosave");
    assert_ne!(recovered.working_commit_id, first_id);
    assert_close(recovered.settings.grade_nodes[0].basic.exposure_stops, 0.85);
    assert!(recovered.versions.is_empty());

    let parsed_photo_id: PhotoId = photo_id.parse().expect("parse photo id");
    let recovered_id: RecipeCommitId = recovered
        .working_commit_id
        .parse()
        .expect("parse recovered working id");
    let recovered_record = session
        .catalog
        .recipe_commit(parsed_photo_id, recovered_id)
        .expect("read recovered working commit")
        .expect("recovered working commit exists");
    let first_id: RecipeCommitId = first_id.parse().expect("parse first working id");
    assert_eq!(recovered_record.commit.parents(), &[first_id]);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove autosave conflict fixture");
}

#[test]
fn autosave_rebases_a_stale_expected_working_head_without_losing_the_draft() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            "",
            "",
            &ffi_parameters(0.15, 1.0, [0.0; 2], 1.0),
            1_000,
        )
        .expect("create initial working autosave");
    let first_id = first.working_commit_id;

    // A controller has already captured the first head for its current
    // draft, then another local state task publishes an adjacent autosave.
    // This mirrors the normal At(A)-vs-current-B conflict seen in the app:
    // the current draft must be appended to B rather than becoming a
    // permanent save failure.
    let interleaved = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            &first_id,
            &first_id,
            &ffi_parameters(0.4, 1.05, [0.01, 0.0], 0.98),
            1_250,
        )
        .expect("publish adjacent autosave");
    let interleaved_id = interleaved.working_commit_id;

    let recovered = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            &first_id,
            &first_id,
            &ffi_parameters(0.85, 1.15, [0.02, -0.01], 0.94),
            1_500,
        )
        .expect("rebase stale expected working autosave");
    assert_ne!(recovered.working_commit_id, interleaved_id);
    assert_close(recovered.settings.grade_nodes[0].basic.exposure_stops, 0.85);
    assert!(recovered.versions.is_empty());

    let parsed_photo_id: PhotoId = photo_id.parse().expect("parse photo id");
    let recovered_id: RecipeCommitId = recovered
        .working_commit_id
        .parse()
        .expect("parse recovered working id");
    let recovered_record = session
        .catalog
        .recipe_commit(parsed_photo_id, recovered_id)
        .expect("read recovered working commit")
        .expect("recovered working commit exists");
    let interleaved_id: RecipeCommitId = interleaved_id
        .parse()
        .expect("parse interleaved working id");
    assert_eq!(recovered_record.commit.parents(), &[interleaved_id]);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove stale expected autosave fixture");
}

#[test]
fn incompatible_working_recipe_requires_confirmation_then_resets_to_neutral_v1() {
    let (root, session, photo_id_text, source_path) = test_edit_session();
    let photo_id: PhotoId = photo_id_text.parse().expect("photo id");
    let current = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
        .expect("create current Recipe v1 snapshot");
    let incompatible =
        RecipeSnapshot::new(CURRENT_RECIPE_SCHEMA_VERSION + 1, current.layers().to_vec())
            .expect("create future development Recipe snapshot");

    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: RecipeCommit::new(
                RecipeCommitId::new_v7(),
                RecipeId::new_v7(),
                Vec::new(),
                incompatible,
                Some("Old development working edit".to_owned()),
                100,
            )
            .expect("create old working edit"),
            update_refs: vec![RecipeRefTarget {
                name: WORKING_RECIPE_REF.to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("persist incompatible working edit");

    let error = session
        .photo_edit_state(&photo_id_text, &source_path)
        .expect_err("old working Recipe requires an explicit reset");
    assert!(
        error
            .to_string()
            .starts_with("incompatible development Recipe:")
    );
    assert_eq!(
        session
            .catalog
            .recipe_commits(photo_id)
            .expect("read unchanged old edits")
            .len(),
        1
    );

    let reset = session
        .reset_incompatible_photo_edit_history(&photo_id_text, &source_path)
        .expect("reset after user confirmation");
    assert!(!reset.has_working_version);
    assert!(reset.working_commit_id.is_empty());
    assert_eq!(reset.settings.grade_nodes.len(), 1);
    assert!(
        session
            .catalog
            .recipe_commits(photo_id)
            .expect("old edits deleted only after reset")
            .is_empty()
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn stale_working_recipe_digest_is_repaired_without_blocking_autosave() {
    let (root, session, photo_id_text, source_path) = test_edit_session();
    let photo_id: PhotoId = photo_id_text.parse().expect("photo id");
    session
        .save_basic_edit_version_at(
            &photo_id_text,
            &source_path,
            "",
            &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
            "Temporary working edit",
            100,
        )
        .expect("create working edit");
    drop(session);

    let connection =
        Connection::open(root.join("catalog.sqlite")).expect("open fixture Catalog directly");
    assert_eq!(
        connection
            .execute(
                "UPDATE recipe_commits SET snapshot_digest = zeroblob(32) WHERE photo_id = ?1",
                params![photo_id.as_bytes().as_slice()],
            )
            .expect("corrupt only the fixture Recipe digest"),
        1
    );
    drop(connection);

    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen edited fixture");
    let state = session
        .photo_edit_state(&photo_id_text, &source_path)
        .expect("valid Recipe semantics repair a stale redundant digest");
    assert!(state.has_working_version);
    assert!(!state.working_commit_id.is_empty());
    assert!(
        session
            .catalog
            .recipe_commits(photo_id)
            .expect("read repaired Recipe history")
            .iter()
            .all(|record| record.snapshot_digest != [0; 32])
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

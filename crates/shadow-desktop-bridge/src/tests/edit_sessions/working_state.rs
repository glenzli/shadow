//! Working-recipe autosave, conflict recovery, and compatibility contracts.

use rusqlite::{Connection, params};
use shadow_catalog::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget};
use shadow_domain::{
    CURRENT_RECIPE_SCHEMA_VERSION, EntityId, PhotoFoundationNode, PhotoId, RawFoundationDenoise,
    RawFoundationDenoiseModel, RawTemperatureTint, RawWhiteBalance, RecipeCommit, RecipeCommitId,
    RecipeId, RecipeInputSettings, RecipeOpticsSettings, RecipeSnapshot,
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
#[allow(clippy::float_cmp, clippy::too_many_lines)] // Exact FFI values across one save/reopen transaction.
fn autosave_and_reopen_preserve_authored_foundation_white_balance() {
    let (root, session, photo_id_text, source_path) = test_edit_session();
    let photo_id: PhotoId = photo_id_text.parse().expect("photo id");
    let manual_white_balance = RawWhiteBalance::temperature_tint(
        RawTemperatureTint::new(6_200, -8).expect("manual temperature/tint"),
    );
    let initial_draft = GradeStackDraft {
        foundation: PhotoFoundationNode::new(
            RecipeInputSettings::new(RecipeOpticsSettings::default())
                .with_raw_white_balance(manual_white_balance),
        ),
        raw_ai_denoise: RawFoundationDenoise::enabled(
            RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
        ),
        ..GradeStackDraft::default()
    };
    let initial_snapshot =
        grade_stack_recipe_v1_snapshot(&initial_draft, None).expect("manual Foundation Recipe");
    let recipe_id = RecipeId::new_v7();
    let initial_commit_id = RecipeCommitId::new_v7();
    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: RecipeCommit::new(
                initial_commit_id,
                recipe_id,
                Vec::new(),
                initial_snapshot,
                None,
                1_000,
            )
            .expect("manual Foundation commit"),
            update_refs: vec![RecipeRefTarget {
                name: WORKING_RECIPE_REF.to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("persist manual Foundation working Recipe");

    let mut state = session
        .photo_edit_state(&photo_id_text, &source_path)
        .expect("open manual Foundation state");
    assert_eq!(state.settings.foundation.raw_white_balance_mode, 1);
    assert_eq!(state.settings.foundation.temperature_kelvin, 6_200);
    assert_eq!(state.settings.foundation.tint, -8);
    assert!(state.settings.foundation.raw_ai_denoise_enabled);
    assert_eq!(state.settings.foundation.raw_ai_denoise_model, 0);
    state.settings.foundation.temperature_kelvin = 4_300;
    state.settings.foundation.tint = 18;
    let authored_white_balance = RawWhiteBalance::temperature_tint(
        RawTemperatureTint::new(4_300, 18).expect("authored temperature/tint"),
    );
    state.settings.grade_nodes[0]
        .basic
        .white_balance_temperature = -0.3;
    state.settings.grade_nodes[0].basic.white_balance_tint = 0.2;
    let autosaved = session
        .autosave_basic_edit_working_at(
            &photo_id_text,
            &source_path,
            &state.working_commit_id,
            &state.working_commit_id,
            &state.settings,
            1_500,
        )
        .expect("autosave Grade edit without flattening Foundation");
    let autosaved_commit_id: RecipeCommitId = autosaved
        .working_commit_id
        .parse()
        .expect("autosaved commit id");
    let autosaved_record = session
        .catalog
        .recipe_commit(photo_id, autosaved_commit_id)
        .expect("read autosaved Recipe")
        .expect("autosaved Recipe exists");
    assert_eq!(
        autosaved_record
            .commit
            .snapshot()
            .foundation_node()
            .raw_white_balance(),
        authored_white_balance
    );
    assert!(
        autosaved_record
            .commit
            .snapshot()
            .raw_ai_denoise_node()
            .is_enabled()
    );

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen manual Foundation fixture");
    let restored = reopened
        .photo_edit_state(&photo_id_text, &source_path)
        .expect("restore manual Foundation autosave");
    assert_eq!(
        restored.settings.grade_nodes[0]
            .basic
            .white_balance_temperature,
        -0.3
    );
    assert_eq!(
        restored.settings.grade_nodes[0].basic.white_balance_tint,
        0.2
    );
    assert_eq!(restored.settings.foundation.raw_white_balance_mode, 1);
    assert_eq!(restored.settings.foundation.temperature_kelvin, 4_300);
    assert_eq!(restored.settings.foundation.tint, 18);
    assert!(restored.settings.foundation.raw_ai_denoise_enabled);
    assert_eq!(restored.settings.foundation.raw_ai_denoise_model, 0);
    let restored_commit_id: RecipeCommitId = restored
        .working_commit_id
        .parse()
        .expect("restored commit id");
    let restored_record = reopened
        .catalog
        .recipe_commit(photo_id, restored_commit_id)
        .expect("read restored Recipe")
        .expect("restored Recipe exists");
    assert_eq!(
        restored_record
            .commit
            .snapshot()
            .foundation_node()
            .raw_white_balance(),
        authored_white_balance
    );
    assert!(
        restored_record
            .commit
            .snapshot()
            .raw_ai_denoise_node()
            .is_enabled()
    );

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove manual Foundation autosave fixture");
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
fn autosave_rejects_stale_missing_head_without_replacing_another_session() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let other = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open independent editor");
    let first = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            "",
            "",
            &ffi_parameters(0.4, 1.0, [0.0; 2], 1.0),
            1_000,
        )
        .expect("publish first editor's exposure");

    // The other editor still believes that no working Recipe exists. Its full
    // draft changes contrast, but must not reset the exposure just published.
    let stale_draft = ffi_parameters(0.0, 1.15, [0.0; 2], 1.0);
    for timestamp in [1_500, 1_750] {
        let error = other
            .autosave_basic_edit_working_at(
                &photo_id,
                &source_path,
                "",
                "",
                &stale_draft,
                timestamp,
            )
            .expect_err("stale Missing expectation must remain a conflict, including retry");
        assert!(matches!(
            error.downcast_ref::<shadow_catalog::CatalogError>(),
            Some(shadow_catalog::CatalogError::RecipeRefExpectationMismatch { .. })
        ));
    }
    let preserved = other.photo_edit_state(&photo_id, &source_path).unwrap();
    assert_eq!(preserved.working_commit_id, first.working_commit_id);
    assert_close(preserved.settings.grade_nodes[0].basic.exposure_stops, 0.4);
    assert_close(preserved.settings.grade_nodes[0].basic.contrast_factor, 1.0);
    assert_eq!(
        session
            .catalog
            .recipe_commits(photo_id.parse().unwrap())
            .unwrap()
            .len(),
        1
    );
    // Failure does not consume or mutate the caller's unsaved draft.
    assert_close(stale_draft.grade_nodes[0].basic.contrast_factor, 1.15);

    drop(other);
    drop(session);
    std::fs::remove_dir_all(root).expect("remove missing-head conflict fixture");
}

#[test]
fn autosave_rejects_stale_expected_head_and_accepts_explicitly_reconciled_draft() {
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
        .expect("publish shared starting point");
    let other = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open independent editor");
    let first_id = first.working_commit_id;
    let interleaved = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            &first_id,
            &first_id,
            &ffi_parameters(0.4, 1.0, [0.0; 2], 1.0),
            1_250,
        )
        .expect("first editor changes only exposure");
    let stale_draft = ffi_parameters(0.15, 1.15, [0.0; 2], 1.0);
    let error = other
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            &first_id,
            &first_id,
            &stale_draft,
            1_500,
        )
        .expect_err("second editor must not overwrite exposure with its old full draft");
    assert!(matches!(
        error.downcast_ref::<shadow_catalog::CatalogError>(),
        Some(shadow_catalog::CatalogError::RecipeRefExpectationMismatch { .. })
    ));
    let preserved = other.photo_edit_state(&photo_id, &source_path).unwrap();
    assert_eq!(preserved.working_commit_id, interleaved.working_commit_id);
    assert_close(preserved.settings.grade_nodes[0].basic.exposure_stops, 0.4);
    assert_close(preserved.settings.grade_nodes[0].basic.contrast_factor, 1.0);
    let parsed_photo_id: PhotoId = photo_id.parse().unwrap();
    assert_eq!(
        session
            .catalog
            .recipe_commits(parsed_photo_id)
            .unwrap()
            .len(),
        2
    );

    // Once the caller has deliberately reconciled with the returned current
    // head, the normal exact-CAS path saves both edits and keeps their ancestry.
    let reconciled = other
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            &preserved.working_commit_id,
            &preserved.working_commit_id,
            &ffi_parameters(0.4, 1.15, [0.0; 2], 1.0),
            2_000,
        )
        .expect("save explicitly reconciled adjustments");
    assert_close(reconciled.settings.grade_nodes[0].basic.exposure_stops, 0.4);
    assert_close(
        reconciled.settings.grade_nodes[0].basic.contrast_factor,
        1.15,
    );
    let record = session
        .catalog
        .recipe_commit(
            parsed_photo_id,
            reconciled.working_commit_id.parse().unwrap(),
        )
        .unwrap()
        .unwrap();
    assert_eq!(
        record.commit.parents(),
        &[preserved
            .working_commit_id
            .parse::<RecipeCommitId>()
            .unwrap()]
    );

    drop(other);
    drop(session);
    std::fs::remove_dir_all(root).expect("remove stale-head conflict fixture");
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

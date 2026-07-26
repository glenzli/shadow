//! Edit save, autosave, reopen, checkout, version, and diff contracts.

use super::*;

#[test]
#[allow(clippy::too_many_lines)]
#[cfg(any())]
fn saving_versions_keeps_old_commits_and_moves_working_atomically() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let neutral = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("load neutral state");
    assert!(!neutral.has_working_version);
    assert!(!neutral.is_version_draft);
    assert!(neutral.working_commit_id.is_empty());
    assert!(neutral.recipe_id.is_empty());
    assert_close(neutral.settings.basic.exposure_stops, 0.0);
    assert_close(neutral.settings.basic.contrast_factor, 1.0);
    assert_close(neutral.settings.basic.white_balance_temperature, 0.0);
    assert_close(neutral.settings.basic.white_balance_tint, 0.0);
    assert_close(neutral.settings.basic.saturation_factor, 1.0);
    assert!(neutral.settings.enabled);
    assert_eq!(
        neutral.settings.tone_curve_kind,
        ffi::FfiToneCurveKind::None
    );
    assert!(neutral.settings.tone_curve_master_points.is_empty());
    assert!(neutral.settings.tone_curve_red_points.is_empty());
    assert!(neutral.settings.tone_curve_green_points.is_empty());
    assert!(neutral.settings.tone_curve_blue_points.is_empty());
    assert!(neutral.versions.is_empty());

    let first_parameters = ffi_parameters(0.5, 1.1, [-0.2, -0.1], 0.8);
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &first_parameters,
            "First look",
            1_000,
        )
        .expect("save first version");
    assert!(!first.is_version_draft);
    let first_id = first.working_commit_id.clone();
    let root_version = first.versions.first().expect("root version");
    assert_root_diff(root_version);

    let second_parameters = ffi_parameters(-0.25, 1.3, [0.15, 0.05], 1.2);
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &second_parameters,
            "Second look",
            2_000,
        )
        .expect("save second version");

    assert!(!second.is_version_draft);
    assert_ne!(second.working_commit_id, first_id);
    assert_eq!(second.versions.len(), 2);
    assert!(
        second
            .versions
            .iter()
            .any(|version| version.commit_id == first_id && !version.is_working)
    );
    let working = second
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working version");
    assert_eq!(working.name, "Second look");
    assert_eq!(working.created_at_ms, 2_000);
    assert_eq!(working.parent_commit_ids, std::slice::from_ref(&first_id));
    assert_all_basic_parameters_changed(working);

    let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
    let commits = session
        .catalog
        .recipe_commits(parsed_photo_id)
        .expect("list durable commits");
    assert_eq!(commits.len(), 2);
    let first_record = commits
        .iter()
        .find(|record| record.commit.id().to_string() == first_id)
        .expect("first commit remains durable");
    let second_record = commits
        .iter()
        .find(|record| record.commit.id().to_string() == second.working_commit_id)
        .expect("second commit is durable");
    let first_identity = single_grade_node_recipe_v1_identity(first_record.commit.snapshot())
        .expect("read first graph identity")
        .expect("first graph is non-empty");
    let second_identity = single_grade_node_recipe_v1_identity(second_record.commit.snapshot())
        .expect("read second graph identity")
        .expect("second graph is non-empty");
    assert_eq!(first_identity.grade_node_id, second_identity.grade_node_id);
    assert_eq!(first_identity.render_op_ids, second_identity.render_op_ids);
    assert!(
        session
            .catalog
            .recipe_ref(
                parsed_photo_id,
                &format!("{NAMED_VERSION_REF_PREFIX}{}", second.working_commit_id)
            )
            .expect("read named version ref")
            .is_some()
    );
    let library_head = session
        .catalog
        .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
        .expect("read Library edit head")
        .expect("Library edit head exists");
    let library_commit = session
        .catalog
        .edit_repository_commit(library_head.commit_id)
        .expect("read Library commit")
        .expect("Library commit exists")
        .commit;
    assert_eq!(library_commit.payload().parents.len(), 1);
    assert_eq!(
        library_commit.payload().message.as_deref(),
        Some("Second look")
    );
    let library_root = session
        .catalog
        .edit_object(library_commit.payload().root)
        .expect("read Library root")
        .expect("Library root exists");
    let library_root =
        LibraryRootV1::from_object(&library_root.object).expect("decode Library root");
    let photo_map_id = library_root.photo_recipes.expect("photo map root");
    let photo_map = session
        .catalog
        .edit_object(photo_map_id)
        .expect("read Library photo map")
        .expect("Library photo map exists");
    let photo_map =
        EditEntityMapV1::from_object(&photo_map.object).expect("decode Library photo map");
    let recipe_object_id = photo_map
        .get(&format!("{LIBRARY_PHOTO_EDIT_KEY_PREFIX}{parsed_photo_id}"))
        .expect("current photo is in the Library tree");
    let recipe_object = session
        .catalog
        .edit_object(recipe_object_id)
        .expect("read Library Recipe leaf")
        .expect("Library Recipe leaf exists");
    assert_eq!(recipe_object.object.kind(), EditObjectKind::LegacyRecipe);
    assert_eq!(
        recipe_object
            .object
            .decode::<RecipeCommit>()
            .expect("decode Library Recipe leaf"),
        second_record.commit
    );
    assert!(
        session
            .catalog
            .edit_repository_ref(&format!(
                "{LIBRARY_EDIT_VERSION_REF_PREFIX}{}",
                library_commit.id()
            ))
            .expect("read named Library version")
            .is_some()
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn stale_expected_working_head_cannot_overwrite_a_newer_working_version() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
            "First",
            1_000,
        )
        .expect("save first version");
    let first_id = first.working_commit_id;
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &ffi_parameters(0.5, 1.2, [0.0; 2], 0.8),
            "Second",
            2_000,
        )
        .expect("save second version");
    let second_id = second.working_commit_id;
    let library_head_id = session
        .catalog
        .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
        .expect("read Library head before stale save")
        .expect("Library head exists")
        .commit_id;

    let error = session
        .save_basic_edit_version_at_with_expected(
            &photo_id,
            &source_path,
            &first_id,
            &first_id,
            &ffi_parameters(-0.5, 0.8, [0.0; 2], 1.2),
            "Stale writer",
            3_000,
        )
        .expect_err("stale expected working head must lose the compare-and-swap");
    let state = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("reload state after rejected save");

    assert!(error.to_string().contains("did not match expectation"));
    assert_eq!(state.working_commit_id, second_id);
    assert_eq!(state.versions.len(), 2);
    assert_eq!(
        session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read Library head after stale save")
            .expect("Library head remains")
            .commit_id,
        library_head_id
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

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
    assert_eq!(restored_fine.color_range_enabled, true);
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
fn unsupported_save_base_fails_before_the_working_head_moves() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let photo_id: PhotoId = photo_id.parse().expect("photo id");
    let base_commit_id = RecipeCommitId::new_v7();
    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: RecipeCommit::new(
                base_commit_id,
                RecipeId::new_v7(),
                Vec::new(),
                single_exposure_recipe(
                    CURRENT_RECIPE_SCHEMA_VERSION,
                    BASIC_GRAPH_SCHEMA_VERSION,
                    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                    CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ),
                Some("Unsupported one-node base".to_owned()),
                1_000,
            )
            .expect("unsupported-but-domain-valid base commit"),
            update_refs: vec![RecipeRefTarget {
                name: WORKING_RECIPE_REF.to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("persist unsupported base");

    let error = session
        .save_basic_edit_version_at(
            &photo_id.to_string(),
            &source_path,
            &base_commit_id.to_string(),
            &ffi_parameters(0.5, 1.1, [0.0; 2], 0.9),
            "Must not commit",
            2_000,
        )
        .expect_err("unsupported base must fail before the Catalog transaction");
    assert!(
        error
            .to_string()
            .contains("validate base Grade Stack Recipe v1")
    );
    let working = session
        .catalog
        .recipe_ref(photo_id, WORKING_RECIPE_REF)
        .expect("read unchanged working ref")
        .expect("working ref remains present");
    assert_eq!(working.commit_id, base_commit_id);
    assert_eq!(
        session
            .catalog
            .recipe_commits(photo_id)
            .expect("list commits after rejected save")
            .len(),
        1
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn generic_nonworking_history_does_not_poison_a_successful_save_response() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let photo_id: PhotoId = photo_id.parse().expect("photo id");
    let recipe_id = RecipeId::new_v7();
    let generic_root_id = RecipeCommitId::new_v7();
    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: RecipeCommit::new(
                generic_root_id,
                recipe_id,
                Vec::new(),
                single_exposure_recipe(
                    CURRENT_RECIPE_SCHEMA_VERSION,
                    BASIC_GRAPH_SCHEMA_VERSION,
                    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                    CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ),
                Some("Generic root".to_owned()),
                500,
            )
            .expect("generic root commit"),
            update_refs: Vec::new(),
        })
        .expect("persist generic root without a working ref");
    let generic_child_id = RecipeCommitId::new_v7();
    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: RecipeCommit::new(
                generic_child_id,
                recipe_id,
                vec![generic_root_id],
                branching_merge_recipe(),
                Some("Generic child".to_owned()),
                750,
            )
            .expect("generic child commit"),
            update_refs: Vec::new(),
        })
        .expect("persist generic child without a working ref");

    let saved = session
        .save_basic_edit_version_at(
            &photo_id.to_string(),
            &source_path,
            "",
            &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
            "Supported working root",
            1_000,
        )
        .expect("generic nonworking history must remain displayable");
    let generic_child = saved
        .versions
        .iter()
        .find(|version| version.commit_id == generic_child_id.to_string())
        .expect("generic child version summary");
    assert!(generic_child.changed_basic_parameters.is_empty());
    assert_eq!(generic_child.changed_basic_parameter_count, 0);
    assert!(generic_child.has_other_changes);
    assert!(saved.has_working_version);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
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

#[test]
fn consecutive_version_reports_the_exact_changed_basic_parameter() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first_parameters = ffi_parameters(0.25, 1.1, [0.05, 0.0], 0.9);
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &first_parameters,
            "Base",
            1_000,
        )
        .expect("save root version");
    let first_id = first.working_commit_id;

    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &ffi_parameters(0.75, 1.1, [0.05, 0.0], 0.9),
            "Exposure only",
            2_000,
        )
        .expect("save exposure version");
    let version = second
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working version");

    assert!(!version.is_root);
    assert_eq!(version.parent_commit_ids, [first_id]);
    assert!(!version.recipe_schema_changed);
    assert_eq!(version.grade_nodes_added, 0);
    assert_eq!(version.grade_nodes_removed, 0);
    assert_eq!(version.grade_nodes_moved, 0);
    assert_eq!(version.grade_nodes_modified, 1);
    assert_eq!(version.render_ops_added, 0);
    assert_eq!(version.render_ops_removed, 0);
    assert_eq!(version.render_ops_modified, 1);
    assert_eq!(version.render_op_parameter_blocks_changed, 1);
    assert_eq!(version.changed_basic_parameter_count, 1);
    assert_eq!(version.changed_basic_parameters, ["exposure_stops"]);
    assert!(!version.has_other_changes);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn version_saved_from_an_old_checkout_diffs_against_the_branch_point() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let base_parameters = ffi_parameters(0.25, 1.1, [0.05, 0.0], 0.9);
    let base = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &base_parameters,
            "Branch point",
            1_000,
        )
        .expect("save branch point");
    let base_id = base.working_commit_id;
    let continuation = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &base_id,
            &ffi_parameters(0.75, 1.1, [0.05, 0.0], 0.9),
            "Exposure branch",
            2_000,
        )
        .expect("save first branch");
    let continuation_id = continuation.working_commit_id;
    let durable_before_checkout = session
        .catalog
        .recipe_ref(photo_id.parse().expect("photo id"), WORKING_RECIPE_REF)
        .expect("read durable working ref before checkout")
        .expect("durable working ref exists");
    assert_eq!(
        durable_before_checkout.commit_id.to_string(),
        continuation_id
    );
    let draft = session
        .checkout_basic_edit_version(&photo_id, &source_path, &base_id)
        .expect("check out branch point");
    assert!(draft.is_version_draft);
    assert_eq!(draft.working_commit_id, base_id);
    assert_close(
        draft.settings.basic.exposure_stops,
        base_parameters.basic.exposure_stops,
    );
    assert_close(
        draft.settings.basic.saturation_factor,
        base_parameters.basic.saturation_factor,
    );
    assert_eq!(
        session
            .catalog
            .recipe_ref(photo_id.parse().expect("photo id"), WORKING_RECIPE_REF)
            .expect("read durable working ref after checkout")
            .expect("durable working ref remains")
            .commit_id
            .to_string(),
        continuation_id
    );

    let branch = session
        .save_basic_edit_version_at_with_expected(
            &photo_id,
            &source_path,
            &base_id,
            &continuation_id,
            &ffi_parameters(0.25, 1.1, [0.05, 0.0], 1.2),
            "Saturation branch",
            4_000,
        )
        .expect("save second branch");
    let working = branch
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working branch version");

    assert_eq!(branch.versions.len(), 3);
    assert!(!branch.is_version_draft);
    assert_eq!(working.parent_commit_ids, [base_id]);
    assert_eq!(working.changed_basic_parameter_count, 1);
    assert_eq!(working.changed_basic_parameters, ["saturation_factor"]);
    assert_eq!(working.grade_nodes_modified, 1);
    assert_eq!(working.render_ops_modified, 1);
    assert_eq!(working.render_op_parameter_blocks_changed, 1);
    assert!(!working.has_other_changes);
    assert!(branch.versions.iter().any(|version| {
        version.commit_id == continuation_id
            && version.parent_commit_ids == working.parent_commit_ids
            && version.changed_basic_parameters == ["exposure_stops"]
    }));
    assert_eq!(
        session
            .catalog
            .recipe_ref(photo_id.parse().expect("photo id"), WORKING_RECIPE_REF)
            .expect("read durable branch ref")
            .expect("durable branch ref exists")
            .commit_id
            .to_string(),
        branch.working_commit_id
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn version_diff_rejects_an_unavailable_first_parent() {
    let photo_id = PhotoId::new_v7();
    let missing_parent = RecipeCommitId::new_v7();
    let commit = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        vec![missing_parent],
        basic_recipe_snapshot(BasicEditParameters::default(), None).expect("build test snapshot"),
        Some("Broken edge".to_owned()),
        1_000,
    )
    .expect("build commit with unresolved external parent");
    let record = RecipeCommitRecord {
        photo_id,
        commit,
        snapshot_digest: [0; 32],
    };

    let error = edit_version_diff(&record, std::slice::from_ref(&record))
        .expect_err("missing first parent must fail");

    assert!(matches!(
        error,
        EditVersionDiffError::ParentMissing {
            parent_id,
            ..
        } if parent_id == missing_parent
    ));
    assert!(
        error
            .to_string()
            .starts_with("edit_version_diff.parent_missing:")
    );
}

#[test]
#[allow(clippy::too_many_lines)]
fn checkout_loads_a_nonpersistent_draft_without_moving_durable_heads() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
    let first_parameters = ffi_parameters(1.0, 0.9, [0.25, 0.0], 0.6);
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &first_parameters,
            "Warm branch point",
            1_000,
        )
        .expect("save first version");
    let first_id = first.working_commit_id;
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &ffi_parameters(-1.0, 1.5, [-0.25, 0.0], 1.4),
            "Cool continuation",
            2_000,
        )
        .expect("save second version");
    let second_id = second.working_commit_id;
    let durable_recipe_ref = session
        .catalog
        .recipe_ref(parsed_photo_id, WORKING_RECIPE_REF)
        .expect("read durable working ref")
        .expect("durable working ref exists");
    assert_eq!(durable_recipe_ref.commit_id.to_string(), second_id);
    let durable_library_ref = session
        .catalog
        .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
        .expect("read durable Library head")
        .expect("durable Library head exists");
    let commit_count = session
        .catalog
        .recipe_commits(parsed_photo_id)
        .expect("list commits before checkout")
        .len();

    let checked_out = session
        .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
        .expect("check out first version");

    assert!(checked_out.is_version_draft);
    assert!(checked_out.has_working_version);
    assert_eq!(checked_out.working_commit_id, first_id);
    assert_eq!(checked_out.versions.len(), 2);
    assert_close(checked_out.settings.basic.exposure_stops, 1.0);
    assert_close(checked_out.settings.basic.contrast_factor, 0.9);
    assert_close(checked_out.settings.basic.white_balance_temperature, 0.25);
    assert_close(checked_out.settings.basic.white_balance_tint, 0.0);
    assert_close(checked_out.settings.basic.saturation_factor, 0.6);
    assert_eq!(
        checked_out
            .versions
            .iter()
            .filter(|version| version.is_working)
            .count(),
        1
    );
    assert!(
        checked_out
            .versions
            .iter()
            .any(|version| { version.commit_id == first_id && version.is_working })
    );
    assert_eq!(
        session
            .catalog
            .recipe_ref(parsed_photo_id, WORKING_RECIPE_REF)
            .expect("read working ref after checkout")
            .expect("working ref remains")
            .commit_id,
        durable_recipe_ref.commit_id
    );
    assert_eq!(
        session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read Library head after checkout")
            .expect("Library head remains")
            .commit_id,
        durable_library_ref.commit_id
    );
    assert_eq!(
        session
            .catalog
            .recipe_commits(parsed_photo_id)
            .expect("list commits after checkout")
            .len(),
        commit_count
    );

    let durable_state = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("reload durable state after checkout");
    assert!(!durable_state.is_version_draft);
    assert_eq!(durable_state.working_commit_id, second_id);
    assert_close(durable_state.settings.basic.exposure_stops, -1.0);
    assert_close(durable_state.settings.basic.contrast_factor, 1.5);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn two_grade_node_stack_saves_reopens_diffs_and_checks_out_exact_identity() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let mut first_settings = ffi_parameters(0.25, 1.1, [0.0; 2], 0.95);
    let mut second_grade_node = new_basic_grade_node("Creative finish").expect("second Grade Node");
    second_grade_node.basic.exposure_stops = -0.4;
    second_grade_node.basic.contrast_factor = 1.3;
    second_grade_node.basic.saturation_factor = 1.2;
    first_settings.grade_nodes.push(second_grade_node);

    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &first_settings,
            "Two-node base",
            1_000,
        )
        .expect("save two-layer root");
    let first_id = first.working_commit_id.clone();
    let first_layer_ids = first
        .settings
        .grade_nodes
        .iter()
        .map(|grade_node| grade_node.grade_node_id.clone())
        .collect::<Vec<_>>();
    assert_eq!(first_layer_ids.len(), 2);

    let mut second_settings = first.settings.clone();
    second_settings.grade_nodes[1].basic.exposure_stops = -0.9;
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &second_settings,
            "Second-node exposure",
            2_000,
        )
        .expect("save two-layer child");
    let second_id = second.working_commit_id.clone();
    let working = second
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working multi-layer version");
    assert_eq!(working.changed_basic_parameters, ["exposure_stops"]);
    assert_eq!(working.grade_nodes_modified, 1);
    assert_eq!(working.render_ops_modified, 1);
    assert!(!working.has_other_changes);

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen multi-layer session");
    let reopened_state = reopened
        .photo_edit_state(&photo_id, &source_path)
        .expect("read reopened multi-layer stack");
    assert_eq!(reopened_state.working_commit_id, second_id);
    assert_eq!(reopened_state.settings.grade_nodes.len(), 2);
    assert_eq!(
        reopened_state
            .settings
            .grade_nodes
            .iter()
            .map(|grade_node| grade_node.grade_node_id.clone())
            .collect::<Vec<_>>(),
        first_layer_ids
    );
    assert_close(
        reopened_state.settings.grade_nodes[1].basic.exposure_stops,
        -0.9,
    );

    let checked_out = reopened
        .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
        .expect("checkout two-layer root");
    assert!(checked_out.is_version_draft);
    assert_eq!(checked_out.settings.grade_nodes.len(), 2);
    assert_eq!(
        checked_out
            .settings
            .grade_nodes
            .iter()
            .map(|grade_node| grade_node.grade_node_id.clone())
            .collect::<Vec<_>>(),
        first_layer_ids
    );
    assert_close(
        checked_out.settings.grade_nodes[1].basic.exposure_stops,
        -0.4,
    );

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove multi-layer fixture");
}

#[test]
#[cfg(any())]
fn save_reopen_and_checkout_restore_the_complete_tone_curve() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first_points = [[0.0, 0.02], [0.35, 0.2], [0.7, 0.86], [1.0, 1.0]];
    let second_points = [[0.0, -0.04], [0.35, 0.3], [0.7, 0.74], [1.0, 1.08]];
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &ffi_settings_with_tone(0.2, 1.1, [0.0; 2], 0.95, &first_points),
            "First curve",
            1_000,
        )
        .expect("save first curve");
    let first_id = first.working_commit_id;
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &ffi_settings_with_tone(0.2, 1.1, [0.0; 2], 0.95, &second_points),
            "Second curve",
            2_000,
        )
        .expect("save second curve");
    let current_version = second
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working curve version");
    assert_eq!(current_version.changed_basic_parameters, ["tone_curve"]);
    assert!(!current_version.has_other_changes);

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen desktop session");
    let reopened_state = reopened
        .photo_edit_state(&photo_id, &source_path)
        .expect("read reopened curve");
    assert_eq!(ffi_curve_pairs(&reopened_state.settings), second_points);
    let checked_out = reopened
        .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
        .expect("check out first curve");
    assert!(checked_out.is_version_draft);
    assert_eq!(ffi_curve_pairs(&checked_out.settings), first_points);
    assert_eq!(checked_out.versions.len(), 2);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
#[cfg(any())]
fn save_reopen_and_checkout_restore_grade_node_bypass_without_losing_recipe_data() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let points = [[0.0, -0.02], [0.3, 0.18], [0.75, 0.88], [1.0, 1.06]];
    let enabled = ffi_settings_with_tone(0.7, 1.25, [0.08, -0.04], 0.82, &points);
    let first = session
        .save_basic_edit_version_at(&photo_id, &source_path, "", &enabled, "Enabled look", 1_000)
        .expect("save enabled version");
    let first_id = first.working_commit_id;
    let mut disabled = enabled.clone();
    disabled.enabled = false;
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &disabled,
            "Bypassed look",
            2_000,
        )
        .expect("save bypassed version");
    let second_id = second.working_commit_id.clone();
    let current_version = second
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working bypass version");

    assert_eq!(
        current_version.changed_basic_parameters,
        ["grade_node_enabled"]
    );
    assert!(!current_version.has_other_changes);
    assert!(!second.settings.enabled);
    assert_eq!(ffi_curve_pairs(&second.settings), points);

    let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
    let commits = session
        .catalog
        .recipe_commits(parsed_photo_id)
        .expect("list bypass history");
    let first_record = commits
        .iter()
        .find(|record| record.commit.id().to_string() == first_id)
        .expect("enabled commit remains durable");
    let second_record = commits
        .iter()
        .find(|record| record.commit.id().to_string() == second_id)
        .expect("bypassed commit is durable");
    let first_settings =
        decode_grade_stack_draft_from_recipe_v1_snapshot(first_record.commit.snapshot())
            .expect("decode enabled commit");
    let second_settings =
        decode_grade_stack_draft_from_recipe_v1_snapshot(second_record.commit.snapshot())
            .expect("decode bypassed commit");
    assert!(first_settings.enabled);
    assert!(!second_settings.enabled);
    assert_eq!(first_settings.basic, second_settings.basic);
    assert_eq!(first_settings.tone_curve, second_settings.tone_curve);
    assert_eq!(
        single_grade_node_recipe_v1_identity(first_record.commit.snapshot()).unwrap(),
        single_grade_node_recipe_v1_identity(second_record.commit.snapshot()).unwrap()
    );

    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("reopen desktop session");
    let reopened_state = reopened
        .photo_edit_state(&photo_id, &source_path)
        .expect("read reopened bypass state");
    assert_eq!(reopened_state.working_commit_id, second_id);
    assert!(!reopened_state.settings.enabled);
    assert_eq!(ffi_curve_pairs(&reopened_state.settings), points);

    let checked_out = reopened
        .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
        .expect("check out enabled version");
    assert!(checked_out.is_version_draft);
    assert!(checked_out.settings.enabled);
    assert_eq!(ffi_curve_pairs(&checked_out.settings), points);
    assert_close(checked_out.settings.basic.exposure_stops, 0.7);
    assert_close(checked_out.settings.basic.contrast_factor, 1.25);
    assert_close(checked_out.settings.basic.white_balance_temperature, 0.08);
    assert_close(checked_out.settings.basic.white_balance_tint, -0.04);
    assert_close(checked_out.settings.basic.saturation_factor, 0.82);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

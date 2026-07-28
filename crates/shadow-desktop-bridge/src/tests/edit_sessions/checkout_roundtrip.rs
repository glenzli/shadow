//! Non-persistent checkout and complete recipe restoration contracts.

use shadow_domain::PhotoId;

use crate::{
    open_desktop_session,
    recipe_v1::{
        decode_grade_stack_draft_from_recipe_v1_snapshot, new_basic_grade_node,
        single_grade_node_recipe_v1_identity,
    },
    session_edit_history::{LIBRARY_EDIT_MAIN_REF, WORKING_RECIPE_REF},
    tests::fixtures::{
        edit_session::test_edit_session,
        grade_stack::{assert_close, ffi_parameters},
    },
};

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
fn save_reopen_and_checkout_restore_the_complete_oklab_curve() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first_points = vec![0.0, 0.02, 0.35, 0.2, 0.7, 0.86, 1.0, 1.0];
    let second_points = vec![0.0, 0.0, 0.35, 0.3, 0.7, 0.74, 1.0, 1.0];
    let mut first_settings = ffi_parameters(0.2, 1.1, [0.0; 2], 0.95);
    first_settings.grade_nodes[0]
        .fine
        .oklab_lightness_curve_points = first_points.clone();
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &first_settings,
            "First curve",
            1_000,
        )
        .expect("save first curve");
    let first_id = first.working_commit_id;
    let mut second_settings = first_settings;
    second_settings.grade_nodes[0]
        .fine
        .oklab_lightness_curve_points = second_points.clone();
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first_id,
            &second_settings,
            "Second curve",
            2_000,
        )
        .expect("save second curve");
    let current_version = second
        .versions
        .iter()
        .find(|version| version.is_working)
        .expect("working curve version");
    assert_eq!(
        current_version.changed_basic_parameters,
        ["oklab_lightness_curve"]
    );
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
    assert_eq!(
        reopened_state.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        second_points
    );
    let checked_out = reopened
        .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
        .expect("check out first curve");
    assert!(checked_out.is_version_draft);
    assert_eq!(
        checked_out.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        first_points
    );
    assert_eq!(checked_out.versions.len(), 2);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

#[test]
fn save_reopen_and_checkout_restore_grade_node_bypass_without_losing_recipe_data() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let points = vec![0.0, 0.0, 0.3, 0.18, 0.75, 0.88, 1.0, 1.0];
    let mut enabled = ffi_parameters(0.7, 1.25, [0.08, -0.04], 0.82);
    enabled.grade_nodes[0].fine.oklab_lightness_curve_points = points.clone();
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
    assert_eq!(
        second.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        points
    );

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
    assert_eq!(first_settings.fine, second_settings.fine);
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
    assert_eq!(
        reopened_state.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        points
    );

    let checked_out = reopened
        .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
        .expect("check out enabled version");
    assert!(checked_out.is_version_draft);
    assert!(checked_out.settings.enabled);
    assert_eq!(
        checked_out.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points,
        points
    );
    assert_close(checked_out.settings.basic.exposure_stops, 0.7);
    assert_close(checked_out.settings.basic.contrast_factor, 1.25);
    assert_close(checked_out.settings.basic.white_balance_temperature, 0.08);
    assert_close(checked_out.settings.basic.white_balance_tint, -0.04);
    assert_close(checked_out.settings.basic.saturation_factor, 0.82);

    drop(reopened);
    std::fs::remove_dir_all(root).expect("remove edit fixture");
}

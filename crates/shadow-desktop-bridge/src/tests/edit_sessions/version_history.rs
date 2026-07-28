//! Durable named-version publication, branching, and diff contracts.

use shadow_domain::{EditEntityMapV1, EditObjectKind, LibraryRootV1};

use shadow_bridge::BasicEditParameters;
use shadow_catalog::{
    CommitRecipe, RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget,
};
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, CPU_REFERENCE_IMPLEMENTATION_VERSION,
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
};
use shadow_domain::{
    CURRENT_RECIPE_SCHEMA_VERSION, EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId,
};

use crate::{
    edit_version_diff::{EditVersionDiffError, edit_version_diff},
    ffi,
    recipe_v1::{basic_recipe_snapshot, single_grade_node_recipe_v1_identity},
    session_edit_history::{
        LIBRARY_EDIT_MAIN_REF, LIBRARY_EDIT_VERSION_REF_PREFIX, LIBRARY_PHOTO_EDIT_KEY_PREFIX,
        NAMED_VERSION_REF_PREFIX, WORKING_RECIPE_REF,
    },
    tests::fixtures::{
        edit_session::test_edit_session,
        grade_stack::{
            assert_close, branching_merge_recipe, ffi_parameters, single_exposure_recipe,
        },
    },
};

#[test]
#[allow(clippy::too_many_lines)]
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
    assert!(
        neutral.settings.grade_nodes[0]
            .fine
            .oklab_lightness_curve_points
            .is_empty()
    );
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

fn assert_root_diff(version: &ffi::FfiEditVersion) {
    assert!(version.is_root);
    assert!(version.parent_commit_ids.is_empty());
    assert_eq!(version.grade_nodes_added, 0);
    assert_eq!(version.grade_nodes_removed, 0);
    assert_eq!(version.grade_nodes_moved, 0);
    assert_eq!(version.grade_nodes_modified, 0);
    assert_eq!(version.render_ops_added, 0);
    assert_eq!(version.render_ops_removed, 0);
    assert_eq!(version.render_ops_modified, 0);
    assert_eq!(version.render_op_parameter_blocks_changed, 0);
    assert_eq!(version.changed_basic_parameter_count, 0);
    assert!(version.changed_basic_parameters.is_empty());
    assert!(!version.has_other_changes);
}

fn assert_all_basic_parameters_changed(version: &ffi::FfiEditVersion) {
    assert_eq!(version.changed_basic_parameter_count, 5);
    assert_eq!(
        version.changed_basic_parameters,
        [
            "exposure_stops",
            "contrast_factor",
            "white_balance_temperature",
            "white_balance_tint",
            "saturation_factor",
        ]
    );
    assert_eq!(version.render_op_parameter_blocks_changed, 4);
    assert!(!version.has_other_changes);
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

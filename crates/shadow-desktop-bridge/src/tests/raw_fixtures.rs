//! Environment-backed real DNG workflow coverage.

use super::*;

#[test]
#[ignore = "requires SHADOW_TEST_DNG_FOLDER to contain local RAW fixtures"]
#[allow(clippy::too_many_lines)]
#[cfg(any())]
fn real_dng_folder_pages_metadata_and_loads_visuals_lazily() {
    let folder = std::env::var_os("SHADOW_TEST_DNG_FOLDER").expect("SHADOW_TEST_DNG_FOLDER");
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-bridge-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create desktop bridge fixture");
    let (decision_photo_id, decision_sequence, stack_grade_node_ids) = {
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open desktop session");
        session.begin_folder_scan(1).expect("prepare real DNG scan");
        let report = session
            .scan_folder(Path::new(&folder).to_str().expect("fixture folder"), 1)
            .expect("scan real DNG folder");
        let first_page = session.review_page("", "", 1).expect("first Review page");

        assert!(report.supported_files >= 2);
        assert_eq!(first_page.items.len(), 1);
        assert!(first_page.total_items >= 2);
        assert!(first_page.has_more);
        let page = session
            .review_page("", "", 96)
            .expect("complete Review page");
        let item = page
            .items
            .iter()
            .find(|item| item.has_visual && item.has_technical_observation)
            .expect("at least one JPEG visual has a technical observation");
        assert_eq!(item.decision_head_sequence, 0);
        assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Unflagged);
        assert_eq!(item.decision_rating, 0);
        assert!(item.technical_input_width > 0);
        assert!(item.technical_input_width <= 512);
        assert!(item.technical_input_height > 0);
        assert!(item.technical_input_height <= 512);
        assert_eq!(
            item.technical_preprocessing_version,
            technical_analysis_preprocessing_version()
        );
        assert!((0.0..=1.0).contains(&item.mean_luma));
        assert!(item.laplacian_variance >= 0.0);
        assert!(item.edge_energy >= 0.0);
        let visual = session
            .load_review_visual(&item.visual_handle)
            .expect("load first visual lazily");
        assert!(!visual.requires_frame_receipt);
        assert!(visual.bytes.starts_with(&[0xff, 0xd8]));
        assert!(visual.bytes.ends_with(&[0xff, 0xd9]));

        let decision = session
            .set_review_photo_decision(
                &item.photo_id,
                item.decision_head_sequence,
                ffi::FfiDecisionFlag::Picked,
                3,
            )
            .expect("persist a real-DNG Review decision");
        let refreshed = session
            .review_page("", "", 96)
            .expect("refresh real-DNG Review decision");
        let refreshed_item = refreshed
            .items
            .iter()
            .find(|candidate| candidate.photo_id == item.photo_id)
            .expect("refresh selected real-DNG item");
        assert_eq!(refreshed_item.decision_head_sequence, decision.sequence);
        assert_eq!(refreshed_item.decision_flag, ffi::FfiDecisionFlag::Picked);
        assert_eq!(refreshed_item.decision_rating, 3);

        let edits = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        let first_edit = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request("", edits, true),
            )
            .expect("prepare and render first edited preview");
        let second_edit = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request("", ffi_parameters(0.5, 1.1, [0.05, 0.0], 1.15), true),
            )
            .expect("reuse prepared edit preview session");
        assert!(first_edit.bytes.starts_with(&[0xff, 0xd8]));
        assert!(second_edit.bytes.starts_with(&[0xff, 0xd8]));
        assert_preview_analysis(&first_edit);
        assert_preview_analysis(&second_edit);
        assert_ne!(first_edit.bytes, second_edit.bytes);
        assert_ne!(first_edit.luma_histogram, second_edit.luma_histogram);
        assert_persisted_tone_recipe_and_neutral_before(
            session.as_ref(),
            item,
            &ffi_parameters(0.8, 1.25, [0.08, 0.0], 1.2),
        );
        let stack_grade_node_ids = assert_real_dng_grade_stack_round_trip(session.as_ref(), item);
        assert_eq!(
            session
                .edit_preview_sessions
                .lock()
                .expect("edit preview cache")
                .len(),
            1
        );
        (
            item.photo_id.clone(),
            decision.sequence,
            stack_grade_node_ids,
        )
    };
    {
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen real-DNG desktop session");
        let page = reopened
            .review_page("", "", 96)
            .expect("page persisted real-DNG decision");
        let item = page
            .items
            .iter()
            .find(|item| item.photo_id == decision_photo_id)
            .expect("reopen selected real-DNG item");
        assert_eq!(item.decision_head_sequence, decision_sequence);
        assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Picked);
        assert_eq!(item.decision_rating, 3);
        let edit_state = reopened
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("reopen persisted real-DNG Grade Stack");
        assert_eq!(
            edit_state
                .settings
                .grade_nodes
                .iter()
                .map(|grade_node| grade_node.grade_node_id.clone())
                .collect::<Vec<_>>(),
            stack_grade_node_ids
        );
        assert_eq!(edit_state.settings.grade_nodes.len(), 2);
        assert!(!edit_state.settings.grade_nodes[0].enabled);
    }
    std::fs::remove_dir_all(root).expect("remove desktop bridge fixture");
}

#[cfg(any())]
fn assert_persisted_tone_recipe_and_neutral_before(
    session: &DesktopSession,
    item: &ffi::FfiReviewItem,
    edits: &ffi::FfiEditSettings,
) {
    let photo_id: PhotoId = item.photo_id.parse().expect("photo id");
    let recipe_id = RecipeId::new_v7();
    let first_tone_id = persist_test_tone_recipe(session, photo_id, recipe_id, None, 0.72);
    let mut first_settings = session
        .photo_edit_state(&item.photo_id, &item.source_path)
        .expect("read first Tone Curve settings")
        .settings;
    first_settings.basic = edits.basic.clone();
    let tone_current = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&first_tone_id.to_string(), first_settings.clone(), true),
        )
        .expect("render persisted Tone Curve Recipe");
    let neutral_before_first = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request("", first_settings.clone(), false),
        )
        .expect("render neutral Before independently of working Recipe");
    let mut bypassed_settings = first_settings.clone();
    bypassed_settings.enabled = false;
    let bypassed_current = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&first_tone_id.to_string(), bypassed_settings, true),
        )
        .expect("render the real DNG with its Grade Node bypassed");
    let second_tone_id =
        persist_test_tone_recipe(session, photo_id, recipe_id, Some(first_tone_id), 0.28);
    let mut second_settings = session
        .photo_edit_state(&item.photo_id, &item.source_path)
        .expect("read second Tone Curve settings")
        .settings;
    second_settings.basic = edits.basic.clone();
    let old_base_after_ref_move = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&first_tone_id.to_string(), first_settings, true),
        )
        .expect("render exact old base after working ref moves");
    let new_base_after_ref_move = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&second_tone_id.to_string(), second_settings, true),
        )
        .expect("render new working base explicitly");
    let neutral_before_second = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request("", ffi_parameters(0.0, 1.0, [0.0; 2], 1.0), false),
        )
        .expect("render stable neutral Before after ref move");

    assert_eq!(tone_current.bytes, old_base_after_ref_move.bytes);
    assert_ne!(tone_current.bytes, new_base_after_ref_move.bytes);
    assert_ne!(tone_current.bytes, neutral_before_first.bytes);
    assert_eq!(bypassed_current.bytes, neutral_before_first.bytes);
    assert_eq!(
        bypassed_current.luma_histogram,
        neutral_before_first.luma_histogram
    );
    assert_eq!(neutral_before_first.bytes, neutral_before_second.bytes);
    assert_eq!(
        neutral_before_first.luma_histogram,
        neutral_before_second.luma_histogram
    );
    let state = session
        .photo_edit_state(&item.photo_id, &item.source_path)
        .expect("open Basic surface over persisted Tone Curve");
    assert_eq!(state.working_commit_id, second_tone_id.to_string());
}

#[allow(clippy::too_many_lines)]
#[cfg(any())]
fn assert_real_dng_grade_stack_round_trip(
    session: &DesktopSession,
    item: &ffi::FfiReviewItem,
) -> Vec<String> {
    let base = session
        .photo_edit_state(&item.photo_id, &item.source_path)
        .expect("read real-DNG stack base");
    assert_eq!(base.settings.grade_nodes.len(), 1);

    let mut stacked = base.settings.clone();
    let mut finish = new_basic_grade_node("Real DNG finish").expect("create second Grade Node");
    finish.basic.exposure_stops = 0.85;
    finish.basic.contrast_factor = 1.18;
    finish.basic.saturation_factor = 1.12;
    stacked.grade_nodes.push(finish);

    let ordered = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&base.working_commit_id, stacked.clone(), true),
        )
        .expect("render ordered two-node real-DNG stack");
    let mut reversed = stacked.clone();
    reversed.grade_nodes.swap(0, 1);
    let reverse_order = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&base.working_commit_id, reversed, true),
        )
        .expect("render reversed two-node real-DNG stack");
    assert_ne!(
        ordered.bytes, reverse_order.bytes,
        "Grade Node order must materially control real pixels"
    );

    let single = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&base.working_commit_id, base.settings.clone(), true),
        )
        .expect("render single-node real-DNG baseline");
    stacked.grade_nodes[1].enabled = false;
    let bypassed = session
        .render_basic_edit_preview(
            &item.photo_id,
            &item.source_path,
            &preview_request(&base.working_commit_id, stacked.clone(), true),
        )
        .expect("render real-DNG stack with second Grade Node bypassed");
    assert_eq!(single.bytes, bypassed.bytes);

    let saved = session
        .save_basic_edit_version_at(
            &item.photo_id,
            &item.source_path,
            &base.working_commit_id,
            &stacked,
            "Real DNG two-node stack",
            3_000,
        )
        .expect("save real-DNG two-node stack");
    let saved_id = saved.working_commit_id.clone();
    let saved_grade_node_ids = saved
        .settings
        .grade_nodes
        .iter()
        .map(|grade_node| grade_node.grade_node_id.clone())
        .collect::<Vec<_>>();
    assert_eq!(saved_grade_node_ids.len(), 2);
    assert!(!saved.settings.grade_nodes[1].enabled);

    let restored_base = session
        .checkout_basic_edit_version(&item.photo_id, &item.source_path, &base.working_commit_id)
        .expect("check out single-node real-DNG branch point");
    assert!(restored_base.is_version_draft);
    assert_eq!(restored_base.settings.grade_nodes.len(), 1);
    let restored_stack = session
        .checkout_basic_edit_version(&item.photo_id, &item.source_path, &saved_id)
        .expect("check out saved real-DNG stack");
    assert!(restored_stack.is_version_draft);
    assert_eq!(
        restored_stack
            .settings
            .grade_nodes
            .iter()
            .map(|grade_node| grade_node.grade_node_id.clone())
            .collect::<Vec<_>>(),
        saved_grade_node_ids
    );

    let mut reordered = restored_stack.settings;
    reordered.grade_nodes.swap(0, 1);
    let reordered_state = session
        .save_basic_edit_version_at(
            &item.photo_id,
            &item.source_path,
            &saved_id,
            &reordered,
            "Real DNG reordered stack",
            6_000,
        )
        .expect("save reordered real-DNG stack");
    let expected_ids = reordered_state
        .settings
        .grade_nodes
        .iter()
        .map(|grade_node| grade_node.grade_node_id.clone())
        .collect::<Vec<_>>();
    assert_eq!(
        expected_ids,
        [
            saved_grade_node_ids[1].clone(),
            saved_grade_node_ids[0].clone()
        ]
    );
    assert!(!reordered_state.settings.grade_nodes[0].enabled);
    expected_ids
}

#[cfg(any())]
fn persist_test_tone_recipe(
    session: &DesktopSession,
    photo_id: PhotoId,
    recipe_id: RecipeId,
    parent: Option<RecipeCommitId>,
    midpoint_y: f64,
) -> RecipeCommitId {
    let commit_id = RecipeCommitId::new_v7();
    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: RecipeCommit::new(
                commit_id,
                recipe_id,
                parent.into_iter().collect(),
                basic_recipe_with_tone(
                    BasicEditParameters::default(),
                    &[[0.0, 0.0], [0.5, midpoint_y], [1.0, 1.0]],
                    true,
                ),
                Some("Typed Tone Curve".to_owned()),
                2_000,
            )
            .expect("build Tone Curve commit"),
            update_refs: vec![RecipeRefTarget {
                name: WORKING_RECIPE_REF.to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(
                    parent.map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                ),
            }],
        })
        .expect("persist Tone Curve working Recipe");
    commit_id
}

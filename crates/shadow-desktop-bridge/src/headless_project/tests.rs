//! Shared-service contracts: private Recipe continuity and fail-closed inputs.

use std::{fs, path::Path};

use serde_json::{Value, json};

use super::{execute_json, load_guard, open_desktop_session_at};

fn request(root: &Path, command: Value) -> super::Response {
    let mut envelope = json!({"schema_version": 1});
    envelope["command"] = command;
    execute_json(root, &serde_json::to_vec(&envelope).unwrap())
}

fn success(root: &Path, command: Value) -> Value {
    let response = request(root, command);
    assert!(response.ok, "{response:?}");
    response.result.unwrap()
}

fn fixture(root: &Path) -> String {
    let source = root.join("source.png");
    image::RgbImage::from_pixel(32, 24, image::Rgb([70, 90, 110]))
        .save(&source)
        .unwrap();
    let project = root.join("project");
    success(&project, json!({"op": "init"}));
    success(&project, json!({"op": "import", "source": source}))["photo_id"]
        .as_str()
        .unwrap()
        .to_owned()
}

#[test]
fn unsupported_envelopes_do_not_create_a_project() {
    let root = tempfile::tempdir().unwrap();
    let project = root.path().join("absent");
    for bytes in [
        b"".as_slice(),
        b"{}",
        b"{\"schema_version\":2,\"command\":{\"op\":\"init\"}}",
        b"{\"schema_version\":1,\"command\":{\"op\":\"init\",\"typo\":true}}",
    ] {
        assert!(!execute_json(&project, bytes).ok);
        assert!(!project.exists());
    }
    let oversized = vec![b' '; super::MAX_REQUEST_BYTES + 1];
    assert_eq!(
        execute_json(&project, &oversized).error.unwrap().code,
        "request_too_large"
    );
    assert!(request(&project, json!({"op": "capabilities"})).ok);
    assert!(!project.exists());
}

#[test]
fn cli_edit_preserves_private_stages_and_immutable_parent_identity() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let snapshot = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let guard = serde_json::from_value(snapshot["guard"].clone()).unwrap();
    let session =
        open_desktop_session_at(&project.join("catalog.sqlite"), &project.join("cache")).unwrap();
    let (source, mut settings) = load_guard(&session, &project, &guard).unwrap();
    settings.retouch_enabled = false;
    settings.retouch_spots.push(crate::ffi::FfiRetouchSpot {
        center_x: 0.5,
        center_y: 0.5,
        radius_level_zero_pixels: 4,
        mode: 0,
        source_offset_x_radii: 2.0,
        source_offset_y_radii: 0.0,
        source_rotation_degrees: 0.0,
        source_scale: 1.0,
        source_flip_horizontal: false,
        source_flip_vertical: false,
        feather: 0.5,
        strength: 0.75,
        frequency_radius: 8,
    });
    let first = session
        .commit_edit_tool_draft(
            &photo,
            &source,
            &guard.representation_id,
            "",
            "",
            &guard.expected_active_variant_id,
            &settings,
        )
        .unwrap();
    drop(session);
    let before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let edits = json!([
        {"op": "set_basic", "node_index": 0, "values": {"exposure_stops": 0.5}},
        {"op": "set_curve", "node_index": 0, "channel": "rgb_red",
         "points": [[0, 0], [0.5, 0.6], [1, 1]]}
    ]);
    let preview = success(
        &project,
        json!({"op": "preview", "guard": before["guard"], "edits": edits,
            "destination": root.path().join("private-state-preview.png"), "bit_depth": 16}),
    );
    // Disabled private retouch state is still part of the exact draft identity.
    assert_eq!(
        success(&project, json!({"op": "snapshot", "photo_id": photo})),
        before
    );
    let result = success(
        &project,
        json!({"op": "commit", "guard": before["guard"], "edits": edits}),
    );
    assert_eq!(preview["snapshot_digest"], result["snapshot_digest"]);
    assert_eq!(preview["base_commit_id"], first.commit_id);
    assert_eq!(preview["committed"], false);
    let after = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    assert_eq!(after["guard"]["base_commit_id"], result["commit_id"]);
    assert_eq!(
        before["recipe_snapshot"]["retouch_spots"],
        after["recipe_snapshot"]["retouch_spots"]
    );
    assert_eq!(after["recipe_snapshot"]["retouch_enabled"], false);
    assert_eq!(
        before["editable"]["grade_nodes"][0]["grade_node_id"],
        after["editable"]["grade_nodes"][0]["grade_node_id"]
    );
    let history = success(
        &project,
        json!({"op": "history", "photo_id": photo, "limit": 10}),
    );
    assert_eq!(
        history["entries"][0]["parent_commit_ids"][0],
        first.commit_id
    );
    let old = success(
        &project,
        json!({"op": "snapshot", "photo_id": photo, "commit_id": first.commit_id}),
    );
    assert_eq!(old["recipe_snapshot"], before["recipe_snapshot"]);
}

#[test]
fn invalid_batch_and_changed_managed_original_fail_without_a_commit() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let snapshot = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let response = request(
        &project,
        json!({"op": "commit", "guard": snapshot["guard"],
        "edits": [{"op": "set_basic", "node_index": 0, "values": {"exposure_stops": 0.5}},
            {"op": "remove_node", "node_index": 0}]}),
    );
    assert!(!response.ok);
    assert_eq!(
        success(
            &project,
            json!({"op": "history", "photo_id": photo, "limit": 10})
        )["entries"],
        json!([])
    );
    let original = fs::read_dir(project.join("originals"))
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    fs::write(original, b"modified").unwrap();
    assert_eq!(
        request(&project, json!({"op": "snapshot", "photo_id": photo}))
            .error
            .unwrap()
            .code,
        "source_changed"
    );
    // The user's external input was never opened for writing.
    assert!(image::open(root.path().join("source.png")).is_ok());
}

#[test]
fn source_and_request_limits_are_checked_before_payload_admission() {
    let root = tempfile::tempdir().unwrap();
    let project = root.path().join("project");
    success(&project, json!({"op": "init"}));
    let source = root.path().join("large.png");
    fs::File::create(&source)
        .unwrap()
        .set_len(super::MAX_SOURCE_BYTES + 1)
        .unwrap();
    let response = request(&project, json!({"op": "import", "source": source}));
    assert_eq!(response.error.unwrap().code, "invalid_source");
    assert!(!project.join("originals").exists());
}

#[test]
fn legacy_export_retains_commit_digest_and_distinguishes_rendered_identity() {
    use crate::recipe_v1::{GradeStackDraft, grade_stack_recipe_v1_snapshot};
    use shadow_catalog::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget};
    use shadow_domain::operation::{
        HIGHLIGHT_BLUE_SUPPRESSION_PARAMETER_KEY, HIGHLIGHT_GREEN_SUPPRESSION_PARAMETER_KEY,
        HIGHLIGHT_RED_SUPPRESSION_PARAMETER_KEY, SELECTIVE_TONE_OPERATION_ID,
    };
    use shadow_domain::{EntityId, RecipeCommit, RecipeCommitId, RecipeId, RecipeSnapshot};

    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let mut draft = GradeStackDraft::default();
    draft.fine.selective_tone.highlights = -0.4;
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).unwrap();
    let mut document = serde_json::to_value(snapshot).unwrap();
    let nodes = document["layers"][0]["content"]["graph"]["nodes"]
        .as_array_mut()
        .unwrap();
    let selective = nodes
        .iter_mut()
        .find(|node| node["operation"]["operation_id"] == SELECTIVE_TONE_OPERATION_ID)
        .unwrap();
    let parameters = selective["parameters"].as_object_mut().unwrap();
    for key in [
        HIGHLIGHT_RED_SUPPRESSION_PARAMETER_KEY,
        HIGHLIGHT_GREEN_SUPPRESSION_PARAMETER_KEY,
        HIGHLIGHT_BLUE_SUPPRESSION_PARAMETER_KEY,
    ] {
        assert!(parameters.remove(key).is_some());
    }
    assert_eq!(parameters.len(), 4);
    let legacy: RecipeSnapshot = serde_json::from_value(document).unwrap();
    // This is a supported historical shape, not a corrupt Catalog fixture.
    crate::recipe_v1::compile_recipe_render_plan(&legacy).unwrap();
    let session =
        open_desktop_session_at(&project.join("catalog.sqlite"), &project.join("cache")).unwrap();
    let commit = RecipeCommitId::new_v7();
    session
        .catalog
        .commit_recipe(&CommitRecipe {
            photo_id: photo.parse().unwrap(),
            commit: RecipeCommit::new(
                commit,
                RecipeId::new_v7(),
                Vec::new(),
                legacy,
                Some("Supported four-parameter Selective Tone".to_owned()),
                100,
            )
            .unwrap(),
            update_refs: vec![RecipeRefTarget {
                name: "working".to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .unwrap();
    drop(session);
    let before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let preview = success(
        &project,
        json!({"op": "preview", "guard": before["guard"], "edits": [],
        "destination": root.path().join("legacy-preview.png"), "bit_depth": 8}),
    );
    let exported = success(
        &project,
        json!({"op": "export", "guard": before["guard"],
        "destination": root.path().join("legacy-export.png"), "bit_depth": 8}),
    );
    assert_eq!(exported["commit_id"], commit.to_string());
    assert_eq!(exported["snapshot_digest"], before["snapshot_digest"]);
    assert_eq!(exported["snapshot_digest_role"], "immutable_commit");
    assert_eq!(preview["snapshot_digest_role"], "rendered_draft");
    assert_ne!(
        exported["rendered_snapshot_digest"],
        exported["snapshot_digest"]
    );
    assert_eq!(
        exported["rendered_snapshot_digest"],
        preview["snapshot_digest"]
    );
    let preview_pixels = image::open(root.path().join("legacy-preview.png")).unwrap();
    let export_pixels = image::open(root.path().join("legacy-export.png")).unwrap();
    assert_eq!(preview_pixels.as_bytes(), export_pixels.as_bytes());
    assert_eq!(
        success(&project, json!({"op": "snapshot", "photo_id": photo})),
        before
    );
}

#[test]
fn curve_commands_match_desktop_dto_recipe_and_export_pixels() {
    use crate::recipe_v1::{decode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot};

    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let initial = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let guard: super::protocol::SnapshotGuard =
        serde_json::from_value(initial["guard"].clone()).unwrap();
    let session =
        open_desktop_session_at(&project.join("catalog.sqlite"), &project.join("cache")).unwrap();
    let (source, mut desktop) = load_guard(&session, &project, &guard).unwrap();
    desktop.grade_nodes[0].fine.vibrance = 0.2;
    desktop.grade_nodes[0].basic.exposure_stops = -0.15;
    session
        .commit_edit_tool_draft(
            &photo,
            &source,
            &guard.representation_id,
            "",
            "",
            &guard.expected_active_variant_id,
            &desktop,
        )
        .unwrap();
    drop(session);
    let before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let guard: super::protocol::SnapshotGuard =
        serde_json::from_value(before["guard"].clone()).unwrap();
    let session =
        open_desktop_session_at(&project.join("catalog.sqlite"), &project.join("cache")).unwrap();
    let (source, mut desktop) = load_guard(&session, &project, &guard).unwrap();
    // These are the same five DTO slots populated by the Qt curve controller.
    // Deliberately distinct values catch channel swaps at the JSON boundary.
    let fine = &mut desktop.grade_nodes[0].fine;
    fine.oklab_lightness_curve_points = vec![0.0, 0.0, 0.5, 0.58, 1.0, 1.0];
    fine.rgb_curve_master_points = vec![0.0, 0.0, 0.5, 0.62, 1.0, 1.0];
    fine.rgb_curve_red_points = vec![0.0, 0.0, 0.5, 0.44, 1.0, 1.0];
    fine.rgb_curve_green_points = vec![0.0, 0.0, 0.5, 0.55, 1.0, 1.0];
    fine.rgb_curve_blue_points = vec![0.0, 0.0, 0.5, 0.47, 1.0, 1.0];
    let base = session
        .catalog
        .recipe_commit(
            photo.parse().unwrap(),
            guard.base_commit_id.parse().unwrap(),
        )
        .unwrap()
        .unwrap();
    let expected = grade_stack_recipe_v1_snapshot(
        &decode_grade_stack_draft_recipe_v1(&desktop).unwrap(),
        Some(base.commit.snapshot()),
    )
    .unwrap();
    let mut desktop_renders = Vec::new();
    for depth in [8, 16] {
        let rendered = session
            .render_basic_edit_export_receipt(
                &photo,
                &source,
                &crate::ffi::FfiEditExportRequest {
                    base_commit_id: guard.base_commit_id.clone(),
                    settings: desktop.clone(),
                    use_working_recipe: true,
                    bit_depth: depth,
                },
            )
            .unwrap();
        desktop_renders.push((depth, rendered));
    }
    drop(session);
    let edits = json!([
        {"op": "set_curve", "node_index": 0, "channel": "oklab_lightness", "points": [[0,0],[0.5,0.58],[1,1]]},
        {"op": "set_curve", "node_index": 0, "channel": "rgb_master", "points": [[0,0],[0.5,0.62],[1,1]]},
        {"op": "set_curve", "node_index": 0, "channel": "rgb_red", "points": [[0,0],[0.5,0.44],[1,1]]},
        {"op": "set_curve", "node_index": 0, "channel": "rgb_green", "points": [[0,0],[0.5,0.55],[1,1]]},
        {"op": "set_curve", "node_index": 0, "channel": "rgb_blue", "points": [[0,0],[0.5,0.47],[1,1]]}
    ]);
    for (depth, rendered) in &desktop_renders {
        let output = root
            .path()
            .join(format!("curve-desktop-equivalence-{depth}.png"));
        let preview = success(
            &project,
            json!({"op": "preview", "guard": before["guard"], "edits": edits,
                   "destination": output, "bit_depth": depth}),
        );
        assert_eq!(
            preview["snapshot_digest"],
            crate::digest_hex::encode_hex(&rendered.snapshot_digest)
        );
        let image = image::open(output).unwrap();
        if *depth == 8 {
            assert_eq!(image.to_rgb8().as_raw(), &rendered.raster.bytes);
        } else {
            assert_eq!(image.to_rgb16().as_raw(), &rendered.raster.samples16);
        }
    }
    assert_eq!(
        success(&project, json!({"op": "snapshot", "photo_id": photo})),
        before
    );
    let committed = success(
        &project,
        json!({"op": "commit", "guard": before["guard"], "edits": edits}),
    );
    let reopened = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    assert_eq!(
        reopened["recipe_snapshot"],
        serde_json::to_value(expected).unwrap()
    );
    assert_eq!(reopened["snapshot_digest"], committed["snapshot_digest"]);
    assert_eq!(
        reopened["snapshot_digest"],
        crate::digest_hex::encode_hex(&desktop_renders[0].1.snapshot_digest)
    );
    // Complete Recipe equality includes unaddressed vibrance/exposure and identity.
    assert_eq!(
        reopened["editable"]["grade_nodes"][0]["basic"],
        before["editable"]["grade_nodes"][0]["basic"]
    );
}

#[test]
fn curve_commands_reuse_canonical_validation_and_shared_node_admission() {
    use super::protocol::{CurveChannel, Edit};
    use crate::recipe_v1::{GradeStackDraft, encode_grade_stack_draft_recipe_v1};

    let neutral = encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).unwrap();
    for points in [
        vec![[0.0, f64::NAN], [1.0, 1.0]],
        vec![[0.0, 0.0], [1.0, f64::INFINITY]],
    ] {
        let mut settings = neutral.clone();
        let error = super::edit::apply(
            &mut settings,
            &[Edit::SetCurve {
                node_index: 0,
                channel: CurveChannel::RgbRed,
                points,
            }],
        )
        .unwrap_err();
        assert_eq!(
            error.downcast_ref::<super::Fault>().unwrap().code,
            "invalid_edit"
        );
    }
    for channel in CurveChannel::ALL {
        for edit in [
            Edit::SetCurve {
                node_index: 0,
                channel,
                points: vec![[0.0, 0.0], [0.5, 0.6], [1.0, 1.0]],
            },
            Edit::ResetCurve {
                node_index: 0,
                channel,
            },
        ] {
            let mut settings = neutral.clone();
            // Shared control admission must fail before any curve mutation or resolution.
            settings.grade_nodes[0].shared_layer_id = "shared-node".to_owned();
            let before = super::edit::editable_projection(&settings);
            let error = super::edit::apply(&mut settings, &[edit]).unwrap_err();
            assert_eq!(
                error.downcast_ref::<super::Fault>().unwrap().code,
                "unsupported"
            );
            assert_eq!(super::edit::editable_projection(&settings), before);
        }
    }
}

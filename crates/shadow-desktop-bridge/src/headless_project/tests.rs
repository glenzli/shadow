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
    let result = success(
        &project,
        json!({"op": "commit", "guard": before["guard"],
        "edits": [{"op": "set_basic", "node_index": 0, "values": {"exposure_stops": 0.5}}]}),
    );
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

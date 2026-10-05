//! Real process boundary: every request reopens the persistent application.

use std::{
    io::Write,
    path::Path,
    process::{Command, Stdio},
};

use serde_json::{Value, json};

fn start(project: &Path, command: Value) -> std::process::Child {
    let mut child = Command::new(env!("CARGO_BIN_EXE_shadow-cli"))
        .args(["project"])
        .arg(project)
        .arg("-")
        .env("SHADOW_IMAGE_ACCELERATION", "cpu")
        .env_remove("SHADOW_INFER_BASE_URL")
        .env_remove("SHADOW_INFER_CREDENTIAL_FILE")
        .env_remove("SHADOW_RAW_DECODE_HELPER")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let mut envelope = json!({"schema_version": 1});
    envelope["command"] = command;
    child
        .stdin
        .take()
        .unwrap()
        .write_all(&serde_json::to_vec(&envelope).unwrap())
        .unwrap();
    child
}

fn finish(child: std::process::Child) -> Value {
    let output = child.wait_with_output().unwrap();
    let response: Value = serde_json::from_slice(&output.stdout).unwrap_or_else(|error| {
        panic!(
            "invalid response: {error}; stderr={}",
            String::from_utf8_lossy(&output.stderr)
        )
    });
    assert_eq!(
        output.status.code(),
        Some(if response["ok"] == true { 0 } else { 2 })
    );
    response
}

fn call(project: &Path, command: Value) -> Value {
    finish(start(project, command))
}
fn success(project: &Path, command: Value) -> Value {
    let response = call(project, command);
    assert_eq!(response["ok"], true, "{response}");
    response["result"].clone()
}

fn fixture(root: &Path) -> String {
    let source = root.join("synthetic.png");
    image::RgbImage::from_fn(48, 32, |x, y| {
        image::Rgb([
            u8::try_from(x * 3 + 20).unwrap(),
            u8::try_from(y * 4 + 20).unwrap(),
            80,
        ])
    })
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
fn import_edit_reopen_export_preserves_original_and_rgb16_precision() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let original = std::fs::read(root.path().join("synthetic.png")).unwrap();
    let page = success(&project, json!({"op": "photos", "limit": 10}));
    assert_eq!(page["items"][0]["photo_id"], photo);
    assert_eq!(page["has_more"], false);
    let before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let receipt = success(
        &project,
        json!({"op": "commit", "guard": before["guard"], "edits": [
            {"op": "set_basic", "node_index": 0, "values": {"exposure_stops": 0.35, "saturation_factor": 0.8}},
            {"op": "set_geometry", "values": {"enabled": true, "crop_left": 0.25, "crop_right": 0.75, "quarter_turn": 1}}
        ]}),
    );
    let reopened = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    assert_eq!(receipt["commit_id"], reopened["guard"]["base_commit_id"]);
    assert_eq!(receipt["snapshot_digest"], reopened["snapshot_digest"]);
    for depth in [8, 16] {
        let output = root.path().join(format!("export-{depth}.png"));
        let exported = success(
            &project,
            json!({"op": "export", "guard": reopened["guard"],
            "destination": output, "bit_depth": depth}),
        );
        assert_eq!(exported["commit_id"], receipt["commit_id"]);
        let decoded = image::open(&output).unwrap();
        assert_eq!(
            decoded.width(),
            u32::try_from(exported["width"].as_u64().unwrap()).unwrap()
        );
        assert_eq!(
            decoded.height(),
            u32::try_from(exported["height"].as_u64().unwrap()).unwrap()
        );
        assert_eq!((decoded.width(), decoded.height()), (32, 24));
        if depth == 16 {
            assert_eq!(decoded.color(), image::ColorType::Rgb16);
            assert!(
                decoded
                    .to_rgb16()
                    .as_raw()
                    .iter()
                    .any(|sample| sample % 257 != 0)
            );
        }
        let bytes = std::fs::read(&output).unwrap();
        let repeated = call(
            &project,
            json!({"op": "export", "guard": reopened["guard"],
            "destination": output, "bit_depth": depth}),
        );
        assert_eq!(repeated["error"]["code"], "already_exists");
        assert_eq!(std::fs::read(output).unwrap(), bytes);
    }
    assert_eq!(
        std::fs::read(root.path().join("synthetic.png")).unwrap(),
        original
    );
    let failed = call(
        &project,
        json!({"op": "export", "guard": reopened["guard"],
        "destination": project.join("forbidden.png"), "bit_depth": 8}),
    );
    assert_eq!(failed["error"]["code"], "invalid_destination");
}

#[test]
fn simultaneous_commits_use_cas_and_historical_restore_keeps_both_commits() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let snapshot = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let initial = success(
        &project,
        json!({"op": "commit", "guard": snapshot["guard"], "edits": []}),
    );
    let snapshot = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let a = start(
        &project,
        json!({"op": "commit", "guard": snapshot["guard"], "edits": [
        {"op": "set_basic", "node_index": 0, "values": {"exposure_stops": 0.5}}]}),
    );
    let b = start(
        &project,
        json!({"op": "commit", "guard": snapshot["guard"], "edits": [
        {"op": "set_basic", "node_index": 0, "values": {"exposure_stops": -0.5}}]}),
    );
    let outcomes = [finish(a), finish(b)];
    assert_eq!(
        outcomes.iter().filter(|value| value["ok"] == true).count(),
        1,
        "{outcomes:?}"
    );
    assert_eq!(
        outcomes.iter().find(|value| value["ok"] == false).unwrap()["error"]["code"],
        "conflict"
    );
    let historical = success(
        &project,
        json!({"op": "snapshot", "photo_id": photo, "commit_id": initial["commit_id"]}),
    );
    assert_ne!(
        historical["guard"]["base_commit_id"],
        historical["guard"]["expected_working_commit_id"]
    );
    success(
        &project,
        json!({"op": "commit", "guard": historical["guard"], "edits": []}),
    );
    let latest = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    assert_eq!(
        latest["editable"]["grade_nodes"][0]["basic"]["exposure_stops"],
        0.0
    );
    let first = success(
        &project,
        json!({"op": "history", "photo_id": photo, "limit": 2}),
    );
    assert_eq!(first["has_more"], true);
    let second = success(
        &project,
        json!({"op": "history", "photo_id": photo, "limit": 2, "after": first["next_cursor"]}),
    );
    assert_eq!(second["entries"].as_array().unwrap().len(), 1);
}

#[test]
fn capabilities_work_without_a_project_or_infer_and_keep_limits_honest() {
    let root = tempfile::tempdir().unwrap();
    let project = root.path().join("absent");
    let caps = success(&project, json!({"op": "capabilities"}));
    assert_eq!(caps["cancel"], "unsupported");
    assert_eq!(caps["ai"]["generation"], "unsupported");
    assert_eq!(caps["ai"]["infer_runtime"], "not_probed");
    assert!(!project.exists());
}

#[cfg(unix)]
#[test]
fn native_path_receipts_or_filesystem_rejection_remain_structured() {
    use std::os::unix::{
        ffi::{OsStrExt, OsStringExt},
        fs::symlink,
    };
    let root = tempfile::tempdir().unwrap();
    let project = root
        .path()
        .join(std::ffi::OsString::from_vec(b"project-\xff".to_vec()));
    let response = call(&project, json!({"op": "init"}));
    // APFS rejects these names before a Catalog can be created. Linux's byte
    // paths exercise the full success/export receipt branch below; on macOS
    // the real boundary must still return a JSON error without partial state.
    if cfg!(target_os = "macos") && response["ok"] == false {
        assert_eq!(response["error"]["code"], "io_error");
        assert!(!project.exists());
        return;
    }
    assert_eq!(response["ok"], true, "{response}");
    let initialized = &response["result"];
    assert_eq!(
        initialized["catalog"]["native_path"],
        serde_json::to_value(project.join("catalog.sqlite").as_os_str().as_bytes()).unwrap()
    );
    let source = root.path().join("input.png");
    image::RgbImage::from_pixel(12, 8, image::Rgb([90, 100, 110]))
        .save(&source)
        .unwrap();
    let admitted = success(&project, json!({"op": "import", "source": source}));
    let photo = &admitted["photo_id"];
    let captured = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    success(
        &project,
        json!({"op": "commit", "guard": captured["guard"], "edits": []}),
    );
    let captured = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let real_output = root
        .path()
        .join(std::ffi::OsString::from_vec(b"output-\xfe".to_vec()));
    std::fs::create_dir(&real_output).unwrap();
    let alias = root.path().join("utf8-alias");
    symlink(&real_output, &alias).unwrap();
    let result = success(
        &project,
        json!({"op": "export", "guard": captured["guard"],
        "destination": alias.join("render.png"), "bit_depth": 8}),
    );
    assert_eq!(
        result["destination"]["native_path"],
        serde_json::to_value(
            real_output
                .canonicalize()
                .unwrap()
                .join("render.png")
                .as_os_str()
                .as_bytes()
        )
        .unwrap()
    );
    assert!(image::open(real_output.join("render.png")).is_ok());
}

fn review_update(photo: &str, head: u64, flag: &str, rating: u8) -> Value {
    json!({"op": "set_decision", "photo_id": photo,
        "expected_head_sequence": head, "flag": flag, "rating": rating,
        "provenance": {"authority": "user_directed", "executor": "agent"}})
}

#[test]
#[allow(clippy::too_many_lines)] // One durable cross-process culling/history workflow.
fn user_directed_review_decisions_reopen_filter_and_page_history() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let source_bytes = std::fs::read(root.path().join("synthetic.png")).unwrap();
    let recipe_before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let neutral = success(&project, json!({"op": "decision", "photo_id": photo}));
    assert_eq!(
        neutral,
        json!({"photo_id": photo, "head_sequence": 0, "flag": "unflagged", "rating": 0})
    );
    let empty = success(
        &project,
        json!({"op": "decision_history", "photo_id": photo, "limit": 2}),
    );
    assert!(empty["events"].as_array().unwrap().is_empty());
    assert_eq!(empty["has_more"], false);
    assert!(empty["next_cursor"].is_null());
    let first = success(&project, review_update(&photo, 0, "picked", 4));
    assert_eq!(first["request_provenance"]["executor"], "agent");
    assert_eq!(first["request_provenance_storage"], "response_only");
    assert_eq!(first["event"]["origin"], "human");
    let selected = success(
        &project,
        json!({"op": "photos", "limit": 10,
        "filter": {"flag": "picked", "minimum_rating": 4}}),
    );
    assert_eq!(selected["items"].as_array().unwrap().len(), 1);
    assert_eq!(selected["items"][0]["photo_id"], photo);

    // Interleave another photo's ledger event: sequence cursors are global,
    // while pages and current decision states remain photo-scoped.
    let other_source = root.path().join("other.png");
    image::RgbImage::from_pixel(8, 8, image::Rgb([60, 80, 100]))
        .save(&other_source)
        .unwrap();
    let other = success(&project, json!({"op": "import", "source": other_source}));
    let mut other_request = review_update(other["photo_id"].as_str().unwrap(), 0, "rejected", 2);
    other_request["provenance"]["executor"] = json!("user");
    let other_receipt = success(&project, other_request);
    assert_eq!(other_receipt["request_provenance"]["executor"], "user");
    let second = success(
        &project,
        review_update(
            &photo,
            first["event"]["sequence"].as_u64().unwrap(),
            "unflagged",
            5,
        ),
    );
    assert!(
        second["event"]["sequence"].as_u64().unwrap()
            > first["event"]["sequence"].as_u64().unwrap() + 1
    );
    let third = success(
        &project,
        review_update(
            &photo,
            second["event"]["sequence"].as_u64().unwrap(),
            "picked",
            3,
        ),
    );
    let reopened = success(&project, json!({"op": "decision", "photo_id": photo}));
    assert_eq!(reopened["head_sequence"], third["event"]["sequence"]);
    assert_eq!(reopened["flag"], "picked");
    assert_eq!(reopened["rating"], 3);
    let page = success(
        &project,
        json!({"op": "decision_history", "photo_id": photo, "limit": 2}),
    );
    assert_eq!(page["events"], json!([first["event"], second["event"]]));
    assert_eq!(page["has_more"], true);
    assert_eq!(page["next_cursor"], second["event"]["sequence"]);
    assert_eq!(page["executor_provenance"], "not_recorded");
    let tail = success(
        &project,
        json!({"op": "decision_history", "photo_id": photo,
        "limit": 2, "after": page["next_cursor"]}),
    );
    assert_eq!(tail["events"], json!([third["event"]]));
    assert_eq!(tail["has_more"], false);
    assert!(tail["next_cursor"].is_null());
    let selected = success(
        &project,
        json!({"op": "photos", "limit": 10,
        "filter": {"flag": "picked", "minimum_rating": 4}}),
    );
    assert!(selected["items"].as_array().unwrap().is_empty());
    let rejected = success(
        &project,
        json!({"op": "photos", "limit": 10,
        "filter": {"flag": "rejected", "minimum_rating": 2}}),
    );
    assert_eq!(rejected["items"].as_array().unwrap().len(), 1);
    assert_eq!(rejected["items"][0]["photo_id"], other["photo_id"]);
    let recipe_after = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    assert_eq!(recipe_before["guard"], recipe_after["guard"]);
    assert_eq!(
        recipe_before["recipe_snapshot"],
        recipe_after["recipe_snapshot"]
    );
    assert_eq!(
        std::fs::read(root.path().join("synthetic.png")).unwrap(),
        source_bytes
    );
}

#[test]
#[allow(clippy::too_many_lines)] // All rejected requests must leave the one winning event intact.
fn review_decision_errors_and_concurrent_writers_preserve_exact_ledger() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    for (flag, rating) in [("picked", 6), ("unflagged", 0)] {
        let rejected = call(&project, review_update(&photo, 0, flag, rating));
        assert_eq!(rejected["error"]["code"], "invalid_decision", "{rejected}");
    }
    let mut missing = review_update(&photo, 0, "picked", 4);
    missing.as_object_mut().unwrap().remove("provenance");
    assert_eq!(call(&project, missing)["error"]["code"], "invalid_request");
    for rating in [json!(-1), json!(256)] {
        let mut invalid = review_update(&photo, 0, "picked", 4);
        invalid["rating"] = rating;
        assert_eq!(call(&project, invalid)["error"]["code"], "invalid_request");
    }
    let mut autonomous = review_update(&photo, 0, "picked", 4);
    autonomous["provenance"]["authority"] = json!("autonomous");
    assert_eq!(
        call(&project, autonomous)["error"]["code"],
        "invalid_request"
    );
    let a = start(&project, review_update(&photo, 0, "picked", 4));
    let b = start(&project, review_update(&photo, 0, "rejected", 1));
    let results = [finish(a), finish(b)];
    assert_eq!(
        results.iter().filter(|value| value["ok"] == true).count(),
        1,
        "{results:?}"
    );
    assert_eq!(
        results.iter().find(|value| value["ok"] == false).unwrap()["error"]["code"],
        "conflict"
    );
    let event = &results.iter().find(|value| value["ok"] == true).unwrap()["result"]["event"];
    let current = success(&project, json!({"op": "decision", "photo_id": photo}));
    assert_eq!(current["head_sequence"], event["sequence"]);
    assert_eq!(current["flag"], event["after_flag"]);
    assert_eq!(current["rating"], event["after_rating"]);
    assert_eq!(
        call(&project, review_update(&photo, 0, "unflagged", 2))["error"]["code"],
        "conflict"
    );
    let no_op = review_update(
        &photo,
        current["head_sequence"].as_u64().unwrap(),
        current["flag"].as_str().unwrap(),
        u8::try_from(current["rating"].as_u64().unwrap()).unwrap(),
    );
    assert_eq!(call(&project, no_op)["error"]["code"], "invalid_decision");
    for (after, limit) in [(0, 0), (0, 101), (u64::MAX, 1)] {
        let rejected = call(
            &project,
            json!({"op": "decision_history", "photo_id": photo,
            "after": after, "limit": limit}),
        );
        assert_eq!(rejected["error"]["code"], "invalid_request");
    }
    let history = success(
        &project,
        json!({"op": "decision_history", "photo_id": photo, "limit": 10}),
    );
    assert_eq!(history["events"], json!([event]));
    assert_eq!(
        success(&project, json!({"op": "decision", "photo_id": photo})),
        current
    );
}

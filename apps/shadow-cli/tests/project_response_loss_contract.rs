//! Lost process receipts must direct callers to the state actually published.

use std::{
    io::Write,
    path::Path,
    process::{Command, Output, Stdio},
};

use serde_json::{Value, json};

fn invoke(project: &Path, command: Value, lose_response: bool) -> Output {
    let mut child = Command::new(env!("CARGO_BIN_EXE_shadow-cli"))
        .arg("project")
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
    if lose_response {
        // Execution waits for stdin EOF, so stdout has no reader before publication starts.
        drop(child.stdout.take());
    }
    let mut envelope = json!({"schema_version": 1});
    envelope["command"] = command;
    child
        .stdin
        .take()
        .unwrap()
        .write_all(&serde_json::to_vec(&envelope).unwrap())
        .unwrap();
    child.wait_with_output().unwrap()
}

fn call(project: &Path, command: Value) -> Value {
    let output = invoke(project, command, false);
    let response: Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(
        output.status.code(),
        Some(if response["ok"] == true { 0 } else { 2 }),
        "{output:?}"
    );
    response
}

fn success(project: &Path, command: Value) -> Value {
    let response = call(project, command);
    assert_eq!(response["ok"], true, "{response}");
    response["result"].clone()
}

fn lost_response(project: &Path, command: Value, recovery_routes: &[&str]) {
    let output = invoke(project, command, true);
    assert_eq!(output.status.code(), Some(3), "{output:?}");
    assert!(output.stdout.is_empty());
    let diagnostic = String::from_utf8(output.stderr).unwrap();
    assert!(
        diagnostic.contains("may already have completed"),
        "{diagnostic}"
    );
    assert!(diagnostic.contains("Before retrying"), "{diagnostic}");
    assert!(diagnostic.contains("no deduplication"), "{diagnostic}");
    for route in recovery_routes {
        assert!(diagnostic.contains(route), "{diagnostic}");
    }
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

fn assert_originals_unchanged(root: &Path, expected: &[u8]) {
    assert_eq!(std::fs::read(root.join("synthetic.png")).unwrap(), expected);
    let managed = std::fs::read_dir(root.join("project/originals"))
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    assert_eq!(std::fs::read(managed).unwrap(), expected);
}

#[test]
fn lost_preview_and_export_receipts_preserve_published_pngs_and_reject_overwrite() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let original = std::fs::read(root.path().join("synthetic.png")).unwrap();
    let neutral = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    success(
        &project,
        json!({"op": "commit", "guard": neutral["guard"], "edits": []}),
    );
    let before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let history = success(
        &project,
        json!({"op": "history", "photo_id": photo, "limit": 10}),
    );
    for operation in ["preview", "export"] {
        let destination = root.path().join(format!("{operation}.png"));
        let mut command = json!({"op": operation, "guard": before["guard"],
            "destination": destination, "bit_depth": 16});
        if operation == "preview" {
            command["edits"] = json!([{ "op": "set_basic", "node_index": 0,
                "values": {"exposure_stops": 0.5} }]);
        }
        lost_response(
            &project,
            command.clone(),
            &["requested destination", "preview/export"],
        );
        let pixels = image::open(&destination).unwrap();
        assert_eq!((pixels.width(), pixels.height()), (48, 32));
        assert_eq!(pixels.color(), image::ColorType::Rgb16);
        let bytes = std::fs::read(&destination).unwrap();
        assert_eq!(call(&project, command)["error"]["code"], "already_exists");
        assert_eq!(std::fs::read(destination).unwrap(), bytes);
    }
    assert_eq!(
        success(&project, json!({"op": "snapshot", "photo_id": photo})),
        before
    );
    assert_eq!(
        success(
            &project,
            json!({"op": "history", "photo_id": photo, "limit": 10})
        ),
        history
    );
    assert_originals_unchanged(root.path(), &original);
}

#[test]
fn lost_commit_receipt_requires_snapshot_and_history_before_retry() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let original = std::fs::read(root.path().join("synthetic.png")).unwrap();
    let before = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let command = json!({"op": "commit", "guard": before["guard"], "edits": [
        {"op": "set_basic", "node_index": 0, "values": {"exposure_stops": 0.5}}
    ]});
    lost_response(&project, command.clone(), &["snapshot/history", "edits"]);
    let after = success(&project, json!({"op": "snapshot", "photo_id": photo}));
    let history = success(
        &project,
        json!({"op": "history", "photo_id": photo, "limit": 10}),
    );
    assert_ne!(
        after["guard"]["base_commit_id"],
        before["guard"]["base_commit_id"]
    );
    assert_eq!(
        after["editable"]["grade_nodes"][0]["basic"]["exposure_stops"],
        0.5
    );
    assert_eq!(history["entries"].as_array().unwrap().len(), 1);
    assert_eq!(
        history["entries"][0]["commit_id"],
        after["guard"]["base_commit_id"]
    );
    assert_eq!(call(&project, command)["error"]["code"], "conflict");
    assert_eq!(
        success(
            &project,
            json!({"op": "history", "photo_id": photo, "limit": 10})
        ),
        history
    );
    assert_originals_unchanged(root.path(), &original);
}

#[test]
fn lost_decision_receipt_requires_decision_ledger_before_retry() {
    let root = tempfile::tempdir().unwrap();
    let photo = fixture(root.path());
    let project = root.path().join("project");
    let original = std::fs::read(root.path().join("synthetic.png")).unwrap();
    let command = json!({"op": "set_decision", "photo_id": photo,
        "expected_head_sequence": 0, "flag": "picked", "rating": 4,
        "provenance": {"authority": "user_directed", "executor": "agent"}});
    lost_response(
        &project,
        command.clone(),
        &["decision/decision_history", "review decisions"],
    );
    let state = success(&project, json!({"op": "decision", "photo_id": photo}));
    let events = success(
        &project,
        json!({"op": "decision_history", "photo_id": photo, "limit": 10}),
    );
    assert_eq!(state["flag"], "picked");
    assert_eq!(state["rating"], 4);
    assert_eq!(events["events"].as_array().unwrap().len(), 1);
    assert_eq!(events["events"][0]["sequence"], state["head_sequence"]);
    assert_eq!(call(&project, command)["error"]["code"], "conflict");
    assert_eq!(
        success(
            &project,
            json!({"op": "decision_history", "photo_id": photo, "limit": 10})
        ),
        events
    );
    assert_eq!(
        success(
            &project,
            json!({"op": "history", "photo_id": photo, "limit": 10})
        )["entries"],
        json!([])
    );
    assert_originals_unchanged(root.path(), &original);
}

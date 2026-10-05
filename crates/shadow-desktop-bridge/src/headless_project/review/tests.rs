//! A declaration of user direction must survive the actual JSON boundary.

use crate::headless_project::execute_json;
use serde_json::{Value, json};

#[test]
fn absent_or_autonomous_provenance_is_rejected_before_project_open() {
    let root = tempfile::tempdir().unwrap();
    let project = root.path().join("absent");
    let command = json!({
        "op": "set_decision", "photo_id": "not-opened",
        "expected_head_sequence": 0, "flag": "picked", "rating": 4
    });
    for provenance in [
        None,
        Some(Value::Null),
        Some(json!({"authority": "autonomous", "executor": "agent"})),
        Some(json!({"authority": "user_directed"})),
        Some(json!({"authority": "user_directed", "executor": "inference"})),
        Some(json!({"authority": "user_directed", "executor": "agent", "implicit": true})),
    ] {
        let mut command = command.clone();
        if let Some(value) = provenance {
            command["provenance"] = value;
        }
        let response = execute_json(
            &project,
            &serde_json::to_vec(&json!({
                "schema_version": 1, "command": command
            }))
            .unwrap(),
        );
        assert_eq!(response.error.unwrap().code, "invalid_request");
        assert!(!project.exists());
    }
    for executor in ["user", "agent"] {
        let mut command = command.clone();
        command["provenance"] = json!({"authority": "user_directed", "executor": executor});
        let response = execute_json(
            &project,
            &serde_json::to_vec(&json!({
                "schema_version": 1, "command": command
            }))
            .unwrap(),
        );
        assert_eq!(response.error.unwrap().code, "project_not_initialized");
        assert!(!project.exists());
    }
}

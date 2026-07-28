use super::*;

#[test]
fn settings_snapshot_must_be_a_single_json_object() {
    let normalized = normalized_settings_json(r#"{ "quality": 92, "format": "jpeg" }"#)
        .expect("object settings are accepted");
    let value: serde_json::Value =
        serde_json::from_str(&normalized).expect("normalized settings remain JSON");
    assert_eq!(value["quality"], 92);
    assert!(normalized_settings_json("[]").is_err());
    assert!(normalized_settings_json("not json").is_err());
}

#[test]
fn export_target_requires_complete_absolute_identity() {
    let output_path = std::env::temp_dir().join("shadow-durable-export-test.jpg");
    let valid = ffi::FfiDurableExportTarget {
        photo_id: "photo-id".into(),
        source_path: "/source/raw.nef".into(),
        output_path: output_path.display().to_string(),
    };
    validate_export_target(&valid).expect("absolute target is valid");

    let invalid = ffi::FfiDurableExportTarget {
        photo_id: "photo-id".into(),
        source_path: "/source/raw.nef".into(),
        output_path: "relative.jpg".into(),
    };
    assert!(validate_export_target(&invalid).is_err());
}

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
fn raw_dng_execution_requires_the_frozen_dng_format() {
    ensure_raw_dng_settings(r#"{"schema":"test","format":"dng"}"#)
        .expect("DNG settings are accepted");
    assert!(ensure_raw_dng_settings(r#"{"format":"jpeg"}"#).is_err());
    assert!(ensure_raw_dng_settings("[]").is_err());
}

#[test]
fn export_target_requires_complete_absolute_identity() {
    let output_path = std::env::temp_dir().join("shadow-durable-export-test.jpg");
    let valid = ffi::FfiDurableExportTarget {
        recipe_commit_id: String::new(),
        representation_id: String::new(),
        photo_id: "photo-id".into(),
        source_path: "/source/raw.nef".into(),
        output_path: crate::native_path_ffi::location_to_ffi(&shadow_native_path::native_location(
            &output_path,
        ))
        .expect("project absolute output path"),
    };
    validate_export_target(&valid).expect("absolute target is valid");

    let invalid = ffi::FfiDurableExportTarget {
        recipe_commit_id: String::new(),
        representation_id: String::new(),
        photo_id: "photo-id".into(),
        source_path: "/source/raw.nef".into(),
        output_path: crate::native_path_ffi::location_to_ffi(&shadow_native_path::native_location(
            std::path::Path::new("relative.jpg"),
        ))
        .expect("project relative output path"),
    };
    assert!(validate_export_target(&invalid).is_err());

    let (platform, unix_bytes, windows_units) = match shadow_native_path::current_platform() {
        shadow_domain::Platform::Windows => (
            ffi::FfiNativePathPlatform::OtherUnix,
            vec![b'/', b't', b'm', b'p', b'/', b'o', b'u', b't'],
            Vec::new(),
        ),
        shadow_domain::Platform::MacOs | shadow_domain::Platform::OtherUnix => (
            ffi::FfiNativePathPlatform::Windows,
            Vec::new(),
            vec![
                b'C'.into(),
                b':'.into(),
                b'\\'.into(),
                b'o'.into(),
                b'u'.into(),
                b't'.into(),
            ],
        ),
    };
    let foreign = ffi::FfiDurableExportTarget {
        recipe_commit_id: String::new(),
        representation_id: String::new(),
        photo_id: "photo-id".into(),
        source_path: "/source/raw.nef".into(),
        output_path: ffi::FfiNativePath {
            platform,
            unix_bytes,
            windows_units,
            display_path: "/display/is/not/reopen-identity.jpg".into(),
        },
    };
    assert!(validate_export_target(&foreign).is_err());
}

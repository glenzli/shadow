use std::{ffi::OsString, path::Path};

use super::{build::parse_for_test, layout::validate_external_path};

#[test]
fn build_command_keeps_check_labels_and_verification_distinct() {
    assert_eq!(
        parse_for_test([OsString::from("--check")]).expect("parse check"),
        "Check"
    );
    assert_eq!(
        parse_for_test([OsString::from("focused-startup")]).expect("parse label"),
        r#"Build { validation_label: Some("focused-startup"), verify_startup: false }"#
    );
    assert_eq!(
        parse_for_test([OsString::from("--verify")]).expect("parse verify"),
        "Build { validation_label: None, verify_startup: true }"
    );
    assert_eq!(
        parse_for_test([OsString::from("--verify"), OsString::from("release-gate")])
            .expect("parse verified label"),
        r#"Build { validation_label: Some("release-gate"), verify_startup: true }"#
    );
    assert!(parse_for_test([OsString::from("--verify"), OsString::from("--check")]).is_err());
    assert!(parse_for_test([OsString::from("--check"), OsString::from("--verify")]).is_err());
    assert!(parse_for_test([OsString::from("--check"), OsString::from("extra")]).is_err());
}

#[test]
fn workflow_outputs_must_stay_outside_the_repository() {
    let workspace = if cfg!(windows) {
        Path::new("C:/workspace")
    } else {
        Path::new("/workspace")
    };
    let repository_path = workspace.join("shadow");
    let repository = repository_path.as_path();
    assert!(validate_external_path(repository, &workspace.join("build"), "OUTPUT").is_ok());
    assert!(validate_external_path(repository, &repository.join("build"), "OUTPUT").is_err());
    assert!(validate_external_path(repository, Path::new("relative"), "OUTPUT").is_err());
    let actual_repository = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let canonical_repository = actual_repository.canonicalize().unwrap();
    assert!(
        validate_external_path(
            &canonical_repository,
            &actual_repository.join("uncreated-build-output"),
            "OUTPUT"
        )
        .is_err()
    );
}

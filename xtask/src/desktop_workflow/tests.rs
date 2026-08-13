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
    let repository = Path::new("/workspace/shadow");
    assert!(validate_external_path(repository, Path::new("/workspace/build"), "OUTPUT").is_ok());
    assert!(
        validate_external_path(repository, Path::new("/workspace/shadow/build"), "OUTPUT").is_err()
    );
    assert!(validate_external_path(repository, Path::new("relative"), "OUTPUT").is_err());
}

use std::{ffi::OsString, path::Path};

use super::{build::parse_for_test, layout::validate_external_path};

#[test]
fn build_command_keeps_check_and_validation_labels_distinct() {
    assert_eq!(
        parse_for_test([OsString::from("--check")]).expect("parse check"),
        "Check"
    );
    assert_eq!(
        parse_for_test([OsString::from("focused-startup")]).expect("parse label"),
        r#"Build { validation_label: Some("focused-startup") }"#
    );
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

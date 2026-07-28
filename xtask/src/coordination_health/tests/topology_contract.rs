use super::{
    super::{
        arguments::DEFAULT_STALE_AFTER,
        claims::evaluate_claim,
        topology::{dependency_cycles, paths_overlap},
    },
    claim_fixture::claim,
};
use std::path::PathBuf;

#[test]
fn dependency_cycles_are_reported_without_claiming_both_sides() {
    let reports = [
        evaluate_claim(
            claim("render", &["desktop"]),
            PathBuf::from("render.json"),
            0,
            DEFAULT_STALE_AFTER,
        ),
        evaluate_claim(
            claim("desktop", &["render"]),
            PathBuf::from("desktop.json"),
            0,
            DEFAULT_STALE_AFTER,
        ),
    ];
    assert_eq!(
        dependency_cycles(&reports),
        vec![vec![
            "desktop".to_owned(),
            "render".to_owned(),
            "desktop".to_owned(),
        ]]
    );
}

#[test]
fn overlap_detection_respects_path_components() {
    assert!(paths_overlap(
        "apps/desktop/qml",
        "apps/desktop/qml/EditHistogram.qml"
    ));
    assert!(!paths_overlap("crates/shadow", "crates/shadow-cache"));
}

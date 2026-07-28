use super::{
    super::{arguments::DEFAULT_STALE_AFTER, claims::evaluate_claim},
    claim_fixture::claim,
};
use std::path::PathBuf;

#[test]
fn high_contention_lease_requires_a_first_release() {
    let mut value = claim("facade", &[]);
    value
        .paths
        .push("apps/desktop/src/desktop_backend.cpp".to_owned());
    let report = evaluate_claim(value, PathBuf::from("facade.json"), 0, DEFAULT_STALE_AFTER);
    assert!(
        report
            .findings
            .iter()
            .any(|finding| finding.contains("first_release"))
    );
}

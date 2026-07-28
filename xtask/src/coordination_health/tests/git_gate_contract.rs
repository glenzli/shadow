use super::{
    super::{
        arguments::DEFAULT_STALE_AFTER,
        claims::{HealthReport, evaluate_claim},
        gates::{ensure_bulk_staging_ready, ensure_scoped_commit_ready},
        time::parse_rfc3339_seconds,
    },
    claim_fixture::claim,
};
use std::path::PathBuf;

#[test]
fn scoped_commit_gate_allows_independent_active_claims() {
    let mut active = claim("active", &[]);
    active.paths.push("crates/active".to_owned());
    let report = HealthReport {
        claims: vec![evaluate_claim(
            active,
            PathBuf::from("active.json"),
            parse_rfc3339_seconds("2026-07-26T00:00:00Z").expect("timestamp parses"),
            DEFAULT_STALE_AFTER,
        )],
        ..HealthReport::default()
    };
    assert!(
        ensure_scoped_commit_ready(&report, &["crates/independent/src/lib.rs".to_owned()]).is_ok()
    );
}

#[test]
fn scoped_commit_gate_rejects_a_staged_path_owned_by_an_active_claim() {
    let mut active = claim("active", &[]);
    active.paths.push("crates/active".to_owned());
    let report = HealthReport {
        claims: vec![evaluate_claim(
            active,
            PathBuf::from("active.json"),
            parse_rfc3339_seconds("2026-07-26T00:00:00Z").expect("timestamp parses"),
            DEFAULT_STALE_AFTER,
        )],
        ..HealthReport::default()
    };
    assert!(ensure_scoped_commit_ready(&report, &["crates/active/src/lib.rs".to_owned()]).is_err());
}

#[test]
fn bulk_stage_gate_requires_quiescence_and_an_empty_index() {
    assert!(ensure_bulk_staging_ready(&HealthReport::default(), &[]).is_ok());
    assert!(
        ensure_bulk_staging_ready(&HealthReport::default(), &["already-staged".to_owned()])
            .is_err()
    );
}

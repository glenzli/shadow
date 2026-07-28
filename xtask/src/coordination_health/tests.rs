use super::*;

fn claim(scope: &str, dependencies: &[&str]) -> ClaimFile {
    ClaimFile {
        schema: Some(1),
        scope: scope.to_owned(),
        owner: "test".to_owned(),
        task: Some("focused test task".to_owned()),
        paths: Vec::new(),
        created_at: Some("2026-07-26T00:00:00Z".to_owned()),
        heartbeat_at: Some("2026-07-26T00:00:00Z".to_owned()),
        first_release: None,
        depends_on: dependencies
            .iter()
            .map(|value| (*value).to_owned())
            .collect(),
        status: None,
        kind: None,
        transaction_steward: None,
        participants: Vec::new(),
        resume_condition: None,
        checkpoint: None,
        pause_retained_paths_reason: None,
        overlap_reason: None,
    }
}

#[test]
fn rfc3339_parser_normalizes_offsets_and_leap_days() {
    assert_eq!(parse_rfc3339_seconds("1970-01-01T00:00:00Z"), Ok(0));
    assert_eq!(parse_rfc3339_seconds("1970-01-01T08:00:00+08:00"), Ok(0));
    assert!(parse_rfc3339_seconds("2025-02-29T00:00:00Z").is_err());
    assert!(parse_rfc3339_seconds("2024-02-29T00:00:00Z").is_ok());
}

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

#[test]
fn overlap_detection_respects_path_components() {
    assert!(paths_overlap(
        "apps/desktop/qml",
        "apps/desktop/qml/EditHistogram.qml"
    ));
    assert!(!paths_overlap("crates/shadow", "crates/shadow-cache"));
}

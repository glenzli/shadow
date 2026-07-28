use super::{
    time::{duration_seconds, format_duration, now_unix_seconds, parse_rfc3339_seconds},
    topology::{annotate_dependency_findings, annotate_path_overlaps, dependency_cycles},
};
use serde::Deserialize;
use std::{
    collections::BTreeSet,
    fs, io,
    path::{Path, PathBuf},
    time::Duration,
};

const MAX_FUTURE_CLOCK_SKEW_SECONDS: i64 = 5 * 60;

#[derive(Debug, Deserialize)]
pub(super) struct ClaimFile {
    #[serde(default)]
    pub(super) schema: Option<u32>,
    pub(super) scope: String,
    pub(super) owner: String,
    #[serde(default)]
    pub(super) task: Option<String>,
    #[serde(default)]
    pub(super) paths: Vec<String>,
    #[serde(default)]
    pub(super) created_at: Option<String>,
    #[serde(default)]
    pub(super) heartbeat_at: Option<String>,
    #[serde(default)]
    pub(super) first_release: Option<String>,
    #[serde(default)]
    pub(super) depends_on: Vec<String>,
    #[serde(default)]
    pub(super) status: Option<String>,
    #[serde(default)]
    pub(super) kind: Option<String>,
    #[serde(default)]
    pub(super) transaction_steward: Option<String>,
    #[serde(default)]
    pub(super) participants: Vec<String>,
    #[serde(default)]
    pub(super) resume_condition: Option<String>,
    #[serde(default)]
    pub(super) checkpoint: Option<serde_json::Value>,
    #[serde(default)]
    pub(super) pause_retained_paths_reason: Option<String>,
    #[serde(default)]
    pub(super) overlap_reason: Option<String>,
}

#[derive(Debug)]
pub(super) struct ClaimReport {
    pub(super) claim: ClaimFile,
    pub(super) source: PathBuf,
    pub(super) findings: Vec<String>,
    pub(super) stale: bool,
    pub(super) invalid: bool,
}

#[derive(Debug, Default)]
pub(super) struct HealthReport {
    pub(super) claims: Vec<ClaimReport>,
    pub(super) unreadable_claims: Vec<(PathBuf, String)>,
    pub(super) cycles: Vec<Vec<String>>,
}

pub(super) fn inspect(root: &Path, stale_after: Duration) -> io::Result<HealthReport> {
    let claims_directory = root.join(".agent-coordination/claims");
    if !claims_directory.is_dir() {
        return Ok(HealthReport::default());
    }

    let now = now_unix_seconds()?;
    let mut report = HealthReport::default();
    for entry in fs::read_dir(&claims_directory)? {
        let entry = entry?;
        let source = entry.path();
        if source.extension().and_then(|value| value.to_str()) != Some("json") {
            continue;
        }

        let contents = match fs::read_to_string(&source) {
            Ok(contents) => contents,
            Err(error) => {
                report.unreadable_claims.push((source, error.to_string()));
                continue;
            }
        };
        let claim = match serde_json::from_str::<ClaimFile>(&contents) {
            Ok(claim) => claim,
            Err(error) => {
                report.unreadable_claims.push((source, error.to_string()));
                continue;
            }
        };
        report
            .claims
            .push(evaluate_claim(claim, source, now, stale_after));
    }
    report
        .claims
        .sort_by(|left, right| left.claim.scope.cmp(&right.claim.scope));
    report.cycles = dependency_cycles(&report.claims);
    annotate_dependency_findings(&mut report);
    annotate_path_overlaps(&mut report);
    Ok(report)
}

pub(super) fn evaluate_claim(
    claim: ClaimFile,
    source: PathBuf,
    now: i64,
    stale_after: Duration,
) -> ClaimReport {
    let mut findings = Vec::new();
    let mut invalid = validate_claim_structure(&claim, &source, &mut findings);
    let mut stale = false;
    let created_at = parse_claim_timestamp(
        "created_at",
        claim.created_at.as_deref(),
        now,
        &mut findings,
        &mut invalid,
    );
    let heartbeat_at = parse_claim_timestamp(
        "heartbeat_at",
        claim.heartbeat_at.as_deref(),
        now,
        &mut findings,
        &mut invalid,
    );
    if let (Some(created_at), Some(heartbeat_at)) = (created_at, heartbeat_at)
        && heartbeat_at < created_at
    {
        invalid = true;
        findings.push("heartbeat_at precedes created_at".to_owned());
    }

    let heartbeat = heartbeat_at.or(created_at);
    if let Some(seconds) = heartbeat {
        let age = now.saturating_sub(seconds);
        if age > duration_seconds(stale_after) {
            stale = true;
            findings.push(format!(
                "heartbeat is {} old (threshold {})",
                format_duration(age),
                format_duration(duration_seconds(stale_after)),
            ));
        }
    } else {
        invalid = true;
        findings.push("missing heartbeat_at and created_at".to_owned());
    }

    let high_contention_paths = claim
        .paths
        .iter()
        .filter(|path| is_high_contention_path(path))
        .count();
    let has_first_release = claim
        .first_release
        .as_deref()
        .is_some_and(|value| !value.trim().is_empty());
    if high_contention_paths > 0 && !has_first_release {
        findings.push(format!(
            "contains {high_contention_paths} high-contention path(s) but has no first_release milestone",
        ));
    }
    if high_contention_paths > 3 {
        findings.push(format!(
            "contains {high_contention_paths} high-contention path(s); split implementation from facade wiring if possible",
        ));
    }
    if claim.status.as_deref().unwrap_or("active") == "paused" {
        invalid |= validate_paused_claim(&claim, high_contention_paths, &mut findings);
    }

    ClaimReport {
        claim,
        source,
        findings,
        stale,
        invalid,
    }
}

fn validate_claim_structure(claim: &ClaimFile, source: &Path, findings: &mut Vec<String>) -> bool {
    let mut invalid = false;
    let expected_scope = source.file_stem().and_then(|value| value.to_str());
    if expected_scope != Some(claim.scope.as_str()) {
        invalid = true;
        findings.push("claim filename does not match its scope field".to_owned());
    }
    if !is_coordination_slug(&claim.scope) {
        invalid = true;
        findings.push("scope is not a coordination slug".to_owned());
    }
    if !is_coordination_slug(&claim.owner) {
        invalid = true;
        findings.push("owner is not a coordination slug".to_owned());
    }
    if claim
        .task
        .as_deref()
        .is_none_or(|value| value.trim().is_empty())
    {
        invalid = true;
        findings.push("missing or empty task".to_owned());
    }
    if let Some(schema) = claim.schema
        && schema != 1
    {
        invalid = true;
        findings.push(format!("unsupported claim schema {schema}"));
    }
    if claim.paths.is_empty() {
        invalid = true;
        findings.push("claim has no paths".to_owned());
    }
    let mut unique_paths = BTreeSet::new();
    for path in &claim.paths {
        if !is_valid_claim_path(path) {
            invalid = true;
            findings.push(format!("invalid claimed path {path:?}"));
        }
        if !unique_paths.insert(path) {
            invalid = true;
            findings.push(format!("duplicate claimed path {path:?}"));
        }
    }

    match claim.status.as_deref().unwrap_or("active") {
        "active" | "paused" => {}
        value => {
            invalid = true;
            findings.push(format!("unknown claim status {value:?}"));
        }
    }
    invalid |= validate_claim_relationships(claim, findings);
    invalid
}

fn validate_claim_relationships(claim: &ClaimFile, findings: &mut Vec<String>) -> bool {
    let mut invalid = false;
    match claim.kind.as_deref().unwrap_or("standard") {
        "standard" => {
            if claim.transaction_steward.is_some() || !claim.participants.is_empty() {
                invalid = true;
                findings.push(
                    "standard claim cannot declare transaction steward or participants".to_owned(),
                );
            }
        }
        "transaction" => {
            if !claim
                .transaction_steward
                .as_deref()
                .is_some_and(is_coordination_slug)
            {
                invalid = true;
                findings.push("transaction claim requires a valid transaction_steward".to_owned());
            }
        }
        value => {
            invalid = true;
            findings.push(format!("unknown claim kind {value:?}"));
        }
    }
    for participant in &claim.participants {
        if !is_coordination_slug(participant) {
            invalid = true;
            findings.push(format!("invalid transaction participant {participant:?}"));
        }
    }

    let mut dependencies = BTreeSet::new();
    for dependency in &claim.depends_on {
        if !is_coordination_slug(dependency) {
            invalid = true;
            findings.push(format!("invalid depends_on scope {dependency:?}"));
        }
        if dependency == &claim.scope {
            invalid = true;
            findings.push("claim cannot depend on itself".to_owned());
        }
        if !dependencies.insert(dependency) {
            invalid = true;
            findings.push(format!("duplicate depends_on scope {dependency:?}"));
        }
    }
    if claim
        .first_release
        .as_deref()
        .is_some_and(|value| value.trim().is_empty())
    {
        invalid = true;
        findings.push("first_release is empty".to_owned());
    }
    invalid
}

fn parse_claim_timestamp(
    label: &str,
    value: Option<&str>,
    now: i64,
    findings: &mut Vec<String>,
    invalid: &mut bool,
) -> Option<i64> {
    let value = value?;
    let seconds = match parse_rfc3339_seconds(value) {
        Ok(seconds) => seconds,
        Err(error) => {
            *invalid = true;
            findings.push(format!("invalid {label} timestamp: {error}"));
            return None;
        }
    };
    if seconds > now.saturating_add(MAX_FUTURE_CLOCK_SKEW_SECONDS) {
        *invalid = true;
        findings.push(format!(
            "{label} is {} in the future (maximum tolerated skew is {})",
            format_duration(seconds.saturating_sub(now)),
            format_duration(MAX_FUTURE_CLOCK_SKEW_SECONDS),
        ));
    }
    Some(seconds)
}

fn validate_paused_claim(
    claim: &ClaimFile,
    high_contention_paths: usize,
    findings: &mut Vec<String>,
) -> bool {
    let mut invalid = false;
    if claim
        .resume_condition
        .as_deref()
        .is_none_or(|value| value.trim().is_empty())
    {
        invalid = true;
        findings.push("paused claim requires a non-empty resume_condition".to_owned());
    }
    let Some(checkpoint) = claim
        .checkpoint
        .as_ref()
        .and_then(serde_json::Value::as_object)
    else {
        invalid = true;
        findings.push("paused claim requires an object checkpoint".to_owned());
        return invalid;
    };
    for key in [
        "base_revision",
        "validation",
        "known_failure",
        "next_safe_owner",
    ] {
        if checkpoint
            .get(key)
            .and_then(serde_json::Value::as_str)
            .is_none_or(|value| value.trim().is_empty())
        {
            invalid = true;
            findings.push(format!("paused checkpoint requires a non-empty {key}"));
        }
    }
    let owned_paths = checkpoint
        .get("owned_paths")
        .and_then(serde_json::Value::as_array)
        .and_then(|paths| {
            paths
                .iter()
                .map(|value| value.as_str().map(str::to_owned))
                .collect::<Option<Vec<_>>>()
        });
    if let Some(mut owned_paths) = owned_paths {
        let mut claim_paths = claim.paths.clone();
        owned_paths.sort_unstable();
        claim_paths.sort_unstable();
        if owned_paths != claim_paths {
            invalid = true;
            findings
                .push("paused checkpoint owned_paths must exactly match claimed paths".to_owned());
        }
    } else {
        invalid = true;
        findings.push("paused checkpoint requires an owned_paths string array".to_owned());
    }
    if high_contention_paths > 0
        && claim
            .pause_retained_paths_reason
            .as_deref()
            .is_none_or(|value| value.trim().is_empty())
    {
        findings.push(format!(
            "paused claim retains {high_contention_paths} high-contention path(s) without pause_retained_paths_reason",
        ));
    }
    invalid
}

fn is_coordination_slug(value: &str) -> bool {
    let bytes = value.as_bytes();
    let valid_start = bytes
        .first()
        .is_some_and(|value| value.is_ascii_lowercase() || value.is_ascii_digit());
    (1..=63).contains(&bytes.len())
        && valid_start
        && bytes
            .iter()
            .all(|value| value.is_ascii_lowercase() || value.is_ascii_digit() || *value == b'-')
}

fn is_valid_claim_path(path: &str) -> bool {
    !path.trim().is_empty()
        && !path.starts_with('/')
        && !path.contains('\\')
        && path
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != "..")
}

fn is_high_contention_path(path: &str) -> bool {
    path.ends_with("/src/lib.rs")
        || path.contains("/desktop_backend.")
        || path.ends_with("_controller.cpp")
        || path.ends_with("_controller.hpp")
        || path.starts_with("apps/desktop/qml/")
        || path == "crates/shadow-catalog/src/lib.rs"
}

impl HealthReport {
    pub(super) fn has_stale_or_invalid_claim(&self) -> bool {
        !self.unreadable_claims.is_empty()
            || self
                .claims
                .iter()
                .any(|report| report.stale || report.invalid)
    }

    pub(super) fn has_strict_violation(&self) -> bool {
        !self.unreadable_claims.is_empty()
            || self
                .claims
                .iter()
                .any(|report| report.stale || report.invalid || !report.findings.is_empty())
    }
}

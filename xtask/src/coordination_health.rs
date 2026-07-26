//! Non-destructive health checks for the shared-workspace coordination area.
//!
//! Claims deliberately remain owned by their author until that author releases
//! them. This module can flag a stale or oversized lease, but it never deletes,
//! rewrites, stages, or takes over a claim. That keeps a failed agent from
//! becoming a data-loss event while still making a blocked integration visible.

use serde::Deserialize;
use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
    process::Command,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

const DEFAULT_STALE_AFTER: Duration = Duration::from_mins(30);
const MAX_FUTURE_CLOCK_SKEW_SECONDS: i64 = 5 * 60;

#[derive(Debug, Default)]
struct HealthArguments {
    root: Option<PathBuf>,
    stale_after: Duration,
    fail_on_stale: bool,
    gate: GateMode,
    strict: bool,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
enum GateMode {
    #[default]
    None,
    ScopedCommit,
    BulkStage,
}

impl HealthArguments {
    fn parse(arguments: impl IntoIterator<Item = OsString>) -> io::Result<Self> {
        let mut arguments = arguments.into_iter();
        let mut parsed = Self {
            stale_after: DEFAULT_STALE_AFTER,
            ..Self::default()
        };

        while let Some(argument) = arguments.next() {
            match argument.to_string_lossy().as_ref() {
                "--root" => {
                    let value = arguments.next().ok_or_else(|| {
                        invalid_argument("coordination-health --root requires a path")
                    })?;
                    parsed.root = Some(PathBuf::from(value));
                }
                "--stale-after-minutes" => {
                    let value = arguments.next().ok_or_else(|| {
                        invalid_argument(
                            "coordination-health --stale-after-minutes requires a whole number",
                        )
                    })?;
                    let minutes = value.to_string_lossy().parse::<u64>().map_err(|_| {
                        invalid_argument("stale-after-minutes must be a whole number")
                    })?;
                    if minutes == 0 {
                        return Err(invalid_argument("stale-after-minutes must be positive"));
                    }
                    parsed.stale_after = Duration::from_secs(minutes.saturating_mul(60));
                }
                "--fail-on-stale" => parsed.fail_on_stale = true,
                "--commit-gate" => parsed.set_gate(GateMode::ScopedCommit)?,
                "--bulk-stage-gate" => parsed.set_gate(GateMode::BulkStage)?,
                "--strict" => parsed.strict = true,
                "--help" | "-h" => {
                    return Err(invalid_argument(
                        "usage: cargo xtask coordination-health [--root PATH] [--stale-after-minutes N] [--fail-on-stale] [--strict] [--commit-gate] [--bulk-stage-gate]",
                    ));
                }
                other => {
                    return Err(invalid_argument(&format!(
                        "unknown coordination-health argument: {other}",
                    )));
                }
            }
        }

        Ok(parsed)
    }

    fn set_gate(&mut self, requested: GateMode) -> io::Result<()> {
        if self.gate != GateMode::None {
            return Err(invalid_argument(
                "choose either --commit-gate or --bulk-stage-gate",
            ));
        }
        self.gate = requested;
        Ok(())
    }
}

#[derive(Debug, Deserialize)]
struct ClaimFile {
    #[serde(default)]
    schema: Option<u32>,
    scope: String,
    owner: String,
    #[serde(default)]
    task: Option<String>,
    #[serde(default)]
    paths: Vec<String>,
    #[serde(default)]
    created_at: Option<String>,
    #[serde(default)]
    heartbeat_at: Option<String>,
    #[serde(default)]
    first_release: Option<String>,
    #[serde(default)]
    depends_on: Vec<String>,
    #[serde(default)]
    status: Option<String>,
    #[serde(default)]
    kind: Option<String>,
    #[serde(default)]
    transaction_steward: Option<String>,
    #[serde(default)]
    participants: Vec<String>,
    #[serde(default)]
    resume_condition: Option<String>,
    #[serde(default)]
    checkpoint: Option<serde_json::Value>,
    #[serde(default)]
    pause_retained_paths_reason: Option<String>,
    #[serde(default)]
    overlap_reason: Option<String>,
}

#[derive(Debug)]
struct ClaimReport {
    claim: ClaimFile,
    source: PathBuf,
    findings: Vec<String>,
    stale: bool,
    invalid: bool,
}

#[derive(Debug, Default)]
struct HealthReport {
    claims: Vec<ClaimReport>,
    unreadable_claims: Vec<(PathBuf, String)>,
    cycles: Vec<Vec<String>>,
}

pub fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let arguments = HealthArguments::parse(arguments)?;
    let root = arguments.root.unwrap_or_else(repository_root);
    let report = inspect(&root, arguments.stale_after)?;
    print_report(&root, arguments.stale_after, &report);

    match arguments.gate {
        GateMode::None => {}
        GateMode::ScopedCommit => {
            let staged_paths = staged_paths(&root)?;
            ensure_scoped_commit_ready(&report, &staged_paths)?;
            ensure_cached_diff_is_clean(&root)?;
        }
        GateMode::BulkStage => {
            let staged_paths = staged_paths(&root)?;
            ensure_bulk_staging_ready(&report, &staged_paths)?;
        }
    }
    if arguments.fail_on_stale && report.has_stale_or_invalid_claim() {
        return Err(io::Error::other(
            "coordination health found stale or unreadable claims; do not take over automatically",
        ));
    }
    if arguments.strict && report.has_strict_violation() {
        return Err(io::Error::other(
            "coordination health strict mode found a stale, malformed, or advisory claim; resolve or explicitly hand off the scope before proceeding",
        ));
    }

    Ok(())
}

fn repository_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf()
}

fn inspect(root: &Path, stale_after: Duration) -> io::Result<HealthReport> {
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

fn evaluate_claim(
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

fn annotate_dependency_findings(report: &mut HealthReport) {
    let known_scopes = report
        .claims
        .iter()
        .map(|report| report.claim.scope.clone())
        .collect::<BTreeSet<_>>();
    for claim_report in &mut report.claims {
        for dependency in &claim_report.claim.depends_on {
            if !known_scopes.contains(dependency) {
                claim_report.findings.push(format!(
                    "depends_on references inactive or missing scope {dependency:?}",
                ));
            }
        }
    }
    for cycle in &report.cycles {
        if let Some(scope) = cycle.first()
            && let Some(claim_report) = report
                .claims
                .iter_mut()
                .find(|report| &report.claim.scope == scope)
        {
            claim_report
                .findings
                .push(format!("dependency cycle detected: {}", cycle.join(" -> ")));
        }
    }
}

fn annotate_path_overlaps(report: &mut HealthReport) {
    let mut findings = Vec::<(usize, String)>::new();
    for left_index in 0..report.claims.len() {
        for right_index in left_index + 1..report.claims.len() {
            let left = &report.claims[left_index].claim;
            let right = &report.claims[right_index].claim;
            let overlaps = overlapping_paths(&left.paths, &right.paths);
            if overlaps.is_empty() {
                continue;
            }
            let left_explained = left
                .overlap_reason
                .as_deref()
                .is_some_and(|value| !value.trim().is_empty());
            let right_explained = right
                .overlap_reason
                .as_deref()
                .is_some_and(|value| !value.trim().is_empty());
            let explanation = if left_explained || right_explained {
                "explicitly explained overlap"
            } else {
                "unexplained overlap"
            };
            let paths = overlaps.join(", ");
            findings.push((
                left_index,
                format!("{explanation} with scope {:?}: {paths}", right.scope),
            ));
            findings.push((
                right_index,
                format!("{explanation} with scope {:?}: {paths}", left.scope),
            ));
        }
    }
    for (index, finding) in findings {
        report.claims[index].findings.push(finding);
    }
}

fn overlapping_paths(left: &[String], right: &[String]) -> Vec<String> {
    let mut overlaps = BTreeSet::new();
    for left_path in left {
        for right_path in right {
            if paths_overlap(left_path, right_path) {
                overlaps.insert(format!("{left_path} ↔ {right_path}"));
            }
        }
    }
    overlaps.into_iter().collect()
}

fn paths_overlap(left: &str, right: &str) -> bool {
    let left = Path::new(left);
    let right = Path::new(right);
    left == right || left.starts_with(right) || right.starts_with(left)
}

fn dependency_cycles(claims: &[ClaimReport]) -> Vec<Vec<String>> {
    let adjacency = claims
        .iter()
        .map(|report| (report.claim.scope.clone(), report.claim.depends_on.clone()))
        .collect::<BTreeMap<_, _>>();
    let mut states = BTreeMap::<String, VisitState>::new();
    let mut stack = Vec::new();
    let mut seen_cycles = BTreeSet::new();
    let mut cycles = Vec::new();
    for scope in adjacency.keys() {
        visit_dependency(
            scope,
            &adjacency,
            &mut states,
            &mut stack,
            &mut seen_cycles,
            &mut cycles,
        );
    }
    cycles
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum VisitState {
    Visiting,
    Complete,
}

fn visit_dependency(
    scope: &str,
    adjacency: &BTreeMap<String, Vec<String>>,
    states: &mut BTreeMap<String, VisitState>,
    stack: &mut Vec<String>,
    seen_cycles: &mut BTreeSet<String>,
    cycles: &mut Vec<Vec<String>>,
) {
    if matches!(states.get(scope), Some(VisitState::Complete)) {
        return;
    }
    if matches!(states.get(scope), Some(VisitState::Visiting)) {
        let position = stack.iter().position(|candidate| candidate == scope);
        if let Some(position) = position {
            let mut cycle = stack[position..].to_vec();
            cycle.push(scope.to_owned());
            let key = cycle.join("\u{1f}");
            if seen_cycles.insert(key) {
                cycles.push(cycle);
            }
        }
        return;
    }

    states.insert(scope.to_owned(), VisitState::Visiting);
    stack.push(scope.to_owned());
    if let Some(dependencies) = adjacency.get(scope) {
        for dependency in dependencies {
            if adjacency.contains_key(dependency) {
                visit_dependency(dependency, adjacency, states, stack, seen_cycles, cycles);
            }
        }
    }
    stack.pop();
    states.insert(scope.to_owned(), VisitState::Complete);
}

fn print_report(root: &Path, stale_after: Duration, report: &HealthReport) {
    println!(
        "coordination health: root={} active_claims={} stale_after={}",
        root.display(),
        report.claims.len(),
        format_duration(duration_seconds(stale_after)),
    );
    for claim_report in &report.claims {
        let status = if claim_report.invalid {
            "INVALID"
        } else if claim_report.stale {
            "STALE"
        } else if claim_report.findings.is_empty() {
            "HEALTHY"
        } else {
            "ADVISORY"
        };
        println!(
            "[{status}] {} owner={} paths={} source={}",
            claim_report.claim.scope,
            claim_report.claim.owner,
            claim_report.claim.paths.len(),
            claim_report.source.display(),
        );
        for finding in &claim_report.findings {
            println!("  - {finding}");
        }
    }
    for (source, error) in &report.unreadable_claims {
        println!("[INVALID] {}: {error}", source.display());
    }
    if report.claims.is_empty() && report.unreadable_claims.is_empty() {
        println!("[CLEAR] no active claims");
    }
    if !report.cycles.is_empty() {
        println!(
            "[ACTION] dependency cycles are advisory warnings; break them through a single transaction steward or a sequenced handoff, never by claiming both sides."
        );
    }
}

fn staged_paths(root: &Path) -> io::Result<Vec<String>> {
    let output = Command::new("git")
        .current_dir(root)
        .args(["diff", "--cached", "--name-only", "-z"])
        .output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "cannot inspect staged paths: git exited with {}",
            output.status
        )));
    }
    output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|path| !path.is_empty())
        .map(|path| {
            std::str::from_utf8(path)
                .map(str::to_owned)
                .map_err(|_| io::Error::other("staged path is not valid UTF-8"))
        })
        .collect()
}

fn ensure_scoped_commit_ready(report: &HealthReport, staged_paths: &[String]) -> io::Result<()> {
    if !report.unreadable_claims.is_empty() {
        return Err(io::Error::other(
            "commit gate blocks while a claim is unreadable; ownership cannot be verified",
        ));
    }
    if staged_paths.is_empty() {
        return Err(io::Error::other(
            "commit gate found no staged paths; stage one explicit handed-off scope",
        ));
    }

    let mut conflicts = Vec::new();
    for claim_report in &report.claims {
        for staged_path in staged_paths {
            for claimed_path in &claim_report.claim.paths {
                if paths_overlap(staged_path, claimed_path) {
                    conflicts.push(format!(
                        "{}: {staged_path} ↔ {claimed_path}",
                        claim_report.claim.scope
                    ));
                }
            }
        }
    }
    conflicts.sort();
    conflicts.dedup();
    if !conflicts.is_empty() {
        return Err(io::Error::other(format!(
            "commit gate blocks staged paths still owned by active claims: {}",
            conflicts.join("; ")
        )));
    }

    println!(
        "commit gate: passed; {} staged path(s) do not overlap active source claims",
        staged_paths.len()
    );
    Ok(())
}

fn ensure_cached_diff_is_clean(root: &Path) -> io::Result<()> {
    let status = Command::new("git")
        .current_dir(root)
        .args(["diff", "--cached", "--check"])
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "commit gate found cached diff errors; git diff --cached --check exited with {status}"
        )))
    }
}

fn ensure_bulk_staging_ready(report: &HealthReport, staged_paths: &[String]) -> io::Result<()> {
    if !staged_paths.is_empty() {
        return Err(io::Error::other(format!(
            "bulk-stage gate requires an empty index, but {} path(s) are already staged",
            staged_paths.len()
        )));
    }
    if report.claims.is_empty() && report.unreadable_claims.is_empty() {
        println!("bulk-stage gate: passed; index is empty and no active claim remains");
        return Ok(());
    }
    let scopes = report
        .claims
        .iter()
        .map(|report| report.claim.scope.as_str())
        .collect::<Vec<_>>()
        .join(", ");
    Err(io::Error::other(format!(
        "bulk-stage gate blocks while active or unreadable claims exist: {scopes}; use exact-path staging and the scoped commit gate instead"
    )))
}

impl HealthReport {
    fn has_stale_or_invalid_claim(&self) -> bool {
        !self.unreadable_claims.is_empty()
            || self
                .claims
                .iter()
                .any(|report| report.stale || report.invalid)
    }

    fn has_strict_violation(&self) -> bool {
        !self.unreadable_claims.is_empty()
            || self
                .claims
                .iter()
                .any(|report| report.stale || report.invalid || !report.findings.is_empty())
    }
}

fn is_high_contention_path(path: &str) -> bool {
    path.ends_with("/src/lib.rs")
        || path.contains("/desktop_backend.")
        || path.ends_with("_controller.cpp")
        || path.ends_with("_controller.hpp")
        || path.starts_with("apps/desktop/qml/")
        || path == "crates/shadow-catalog/src/lib.rs"
}

fn now_unix_seconds() -> io::Result<i64> {
    let duration = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| io::Error::other(format!("system clock is before Unix epoch: {error}")))?;
    i64::try_from(duration.as_secs())
        .map_err(|_| io::Error::other("system time exceeds i64 Unix seconds"))
}

fn duration_seconds(duration: Duration) -> i64 {
    i64::try_from(duration.as_secs()).unwrap_or(i64::MAX)
}

fn format_duration(seconds: i64) -> String {
    if seconds >= 3600 {
        format!("{}h {}m", seconds / 3600, (seconds % 3600) / 60)
    } else if seconds >= 60 {
        format!("{}m", seconds / 60)
    } else {
        format!("{seconds}s")
    }
}

fn parse_rfc3339_seconds(value: &str) -> Result<i64, String> {
    let bytes = value.as_bytes();
    if bytes.len() < 20
        || bytes.get(4) != Some(&b'-')
        || bytes.get(7) != Some(&b'-')
        || bytes.get(10) != Some(&b'T')
        || bytes.get(13) != Some(&b':')
        || bytes.get(16) != Some(&b':')
    {
        return Err("expected RFC3339 date-time".to_owned());
    }
    let year = parse_component(bytes, 0, 4)?;
    let month = parse_component(bytes, 5, 2)?;
    let day = parse_component(bytes, 8, 2)?;
    let hour = parse_component(bytes, 11, 2)?;
    let minute = parse_component(bytes, 14, 2)?;
    let second = parse_component(bytes, 17, 2)?;
    validate_date_time(year, month, day, hour, minute, second)?;

    let mut timezone_start = 19;
    if bytes.get(timezone_start) == Some(&b'.') {
        timezone_start += 1;
        while bytes.get(timezone_start).is_some_and(u8::is_ascii_digit) {
            timezone_start += 1;
        }
    }
    let offset_seconds = parse_timezone_offset(&bytes[timezone_start..])?;
    let days = days_from_civil(year, month, day);
    let local_seconds = days
        .checked_mul(86_400)
        .and_then(|seconds| seconds.checked_add(i64::from(hour) * 3_600))
        .and_then(|seconds| seconds.checked_add(i64::from(minute) * 60))
        .and_then(|seconds| seconds.checked_add(i64::from(second)))
        .ok_or_else(|| "timestamp exceeds i64 Unix seconds".to_owned())?;
    local_seconds
        .checked_sub(i64::from(offset_seconds))
        .ok_or_else(|| "timestamp offset exceeds i64 Unix seconds".to_owned())
}

fn parse_component(bytes: &[u8], start: usize, length: usize) -> Result<i32, String> {
    let component = bytes
        .get(start..start + length)
        .ok_or_else(|| "timestamp is truncated".to_owned())?;
    if !component.iter().all(u8::is_ascii_digit) {
        return Err("timestamp contains a non-numeric component".to_owned());
    }
    component.iter().try_fold(0_i32, |value, digit| {
        value
            .checked_mul(10)
            .and_then(|value| value.checked_add(i32::from(*digit - b'0')))
            .ok_or_else(|| "timestamp component overflows".to_owned())
    })
}

fn validate_date_time(
    year: i32,
    month: i32,
    day: i32,
    hour: i32,
    minute: i32,
    second: i32,
) -> Result<(), String> {
    if !(1..=12).contains(&month) {
        return Err("month is outside 1..=12".to_owned());
    }
    let month = u32::try_from(month).map_err(|_| "invalid month".to_owned())?;
    let maximum_day = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if is_leap_year(year) => 29,
        2 => 28,
        _ => unreachable!("month was range checked"),
    };
    if !(1..=maximum_day).contains(&day) {
        return Err("day is outside the selected month".to_owned());
    }
    if !(0..=23).contains(&hour) || !(0..=59).contains(&minute) || !(0..=59).contains(&second) {
        return Err("time is outside RFC3339 whole-second bounds".to_owned());
    }
    Ok(())
}

fn parse_timezone_offset(bytes: &[u8]) -> Result<i32, String> {
    match bytes {
        [b'Z'] => Ok(0),
        [
            sign @ (b'+' | b'-'),
            hour_a,
            hour_b,
            b':',
            minute_a,
            minute_b,
        ] => {
            let hour = parse_component(&[*hour_a, *hour_b], 0, 2)?;
            let minute = parse_component(&[*minute_a, *minute_b], 0, 2)?;
            if hour > 23 || minute > 59 {
                return Err("timezone offset is outside RFC3339 bounds".to_owned());
            }
            let seconds = hour * 3_600 + minute * 60;
            Ok(if *sign == b'+' { seconds } else { -seconds })
        }
        _ => Err("timezone must be Z or ±HH:MM".to_owned()),
    }
}

fn is_leap_year(year: i32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

fn days_from_civil(year: i32, month: i32, day: i32) -> i64 {
    let year = i64::from(year) - i64::from(month <= 2);
    let era = if year >= 0 { year } else { year - 399 } / 400;
    let year_of_era = year - era * 400;
    let adjusted_month = i64::from(month) + if month > 2 { -3 } else { 9 };
    let day_of_year = (153 * adjusted_month + 2) / 5 + i64::from(day) - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146_097 + day_of_era - 719_468
}

fn invalid_argument(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

#[cfg(test)]
mod tests {
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
            ensure_scoped_commit_ready(&report, &["crates/independent/src/lib.rs".to_owned()])
                .is_ok()
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
        assert!(
            ensure_scoped_commit_ready(&report, &["crates/active/src/lib.rs".to_owned()]).is_err()
        );
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
}

use super::{
    claims::HealthReport,
    time::{duration_seconds, format_duration},
};
use std::{path::Path, time::Duration};

pub(super) fn print_report(root: &Path, stale_after: Duration, report: &HealthReport) {
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

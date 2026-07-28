use super::{claims::HealthReport, topology::paths_overlap};
use std::{io, path::Path, process::Command};

pub(super) fn staged_paths(root: &Path) -> io::Result<Vec<String>> {
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

pub(super) fn ensure_scoped_commit_ready(
    report: &HealthReport,
    staged_paths: &[String],
) -> io::Result<()> {
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

pub(super) fn ensure_cached_diff_is_clean(root: &Path) -> io::Result<()> {
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

pub(super) fn ensure_bulk_staging_ready(
    report: &HealthReport,
    staged_paths: &[String],
) -> io::Result<()> {
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

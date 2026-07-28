//! Non-destructive health checks for the shared-workspace coordination area.
//!
//! This is the command boundary and navigation index:
//!
//! - [`arguments`] owns CLI admission.
//! - [`claims`] owns claim loading and structural health.
//! - [`topology`] owns dependency and path-overlap analysis.
//! - [`gates`] owns staged Git transaction admission.
//! - [`time`] owns timestamp normalization.
//! - [`presentation`] owns the operator-facing report.
//!
//! Claims deliberately remain owned by their author until that author releases
//! them. The command can flag a stale or oversized lease, but it never deletes,
//! rewrites, stages, or takes over a claim.

mod arguments;
mod claims;
mod gates;
mod presentation;
mod time;
mod topology;

#[cfg(test)]
mod tests;

use arguments::{GateMode, HealthArguments};
use std::{ffi::OsString, io, path::PathBuf};

pub fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let arguments = HealthArguments::parse(arguments)?;
    let root = arguments.root.unwrap_or_else(repository_root);
    let report = claims::inspect(&root, arguments.stale_after)?;
    presentation::print_report(&root, arguments.stale_after, &report);

    match arguments.gate {
        GateMode::None => {}
        GateMode::ScopedCommit => {
            let staged_paths = gates::staged_paths(&root)?;
            gates::ensure_scoped_commit_ready(&report, &staged_paths)?;
            gates::ensure_cached_diff_is_clean(&root)?;
        }
        GateMode::BulkStage => {
            let staged_paths = gates::staged_paths(&root)?;
            gates::ensure_bulk_staging_ready(&report, &staged_paths)?;
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

fn invalid_argument(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

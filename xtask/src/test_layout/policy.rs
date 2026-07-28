//! Absolute repository policy for Rust test ownership and reachability.

use std::{
    collections::{BTreeMap, BTreeSet},
    io,
};

use super::audit::TestLayoutObservation;

pub(super) fn enforce(observation: &TestLayoutObservation) -> io::Result<()> {
    let mut violations = Vec::new();
    append_paths(
        &mut violations,
        "Sibling *_test.rs or *_tests.rs production sources",
        &observation.sibling_test_sources,
        "Let the production owner declare tests.rs or tests/mod.rs.",
    );
    append_identities(
        &mut violations,
        "Inline #[cfg(test)] modules in production Rust",
        &observation.inline_test_modules,
        "Keep only an out-of-line #[cfg(test)] mod tests; registration in the production owner.",
    );
    append_identities(
        &mut violations,
        "Executable test bodies in production Rust",
        &observation.inline_executable_tests,
        "Move ordinary test functions into the owner-adjacent tests.rs or tests/ tree.",
    );
    append_identities(
        &mut violations,
        "Private production source inclusions from crate-level tests",
        &observation.crate_test_source_inclusions,
        "Consume the real public product graph or move the private contract beside its owner.",
    );
    append_identities(
        &mut violations,
        "Permanently disabled Rust test code",
        &observation.permanently_disabled_test_sources,
        "Delete obsolete tests, migrate live contracts, or use #[ignore = \"exact prerequisite\"] for runnable external-fixture contracts.",
    );
    append_identities(
        &mut violations,
        "Implementation hidden in Rust tests/mod.rs facades",
        &observation.test_facade_non_registration_items,
        "Keep the facade registration-only; children import production contracts and named fixtures directly.",
    );
    if violations.is_empty() {
        return Ok(());
    }
    Err(io::Error::other(format!(
        "test-layout strict policy rejected the workspace:\n\n{}",
        violations.join("\n\n")
    )))
}

fn append_paths(output: &mut Vec<String>, title: &str, paths: &BTreeSet<String>, guidance: &str) {
    if paths.is_empty() {
        return;
    }
    let findings = paths
        .iter()
        .map(|path| format!("  - {path}"))
        .collect::<Vec<_>>()
        .join("\n");
    output.push(format!("{title}:\n{findings}\n{guidance}"));
}

fn append_identities(
    output: &mut Vec<String>,
    title: &str,
    findings_by_path: &BTreeMap<String, BTreeSet<String>>,
    guidance: &str,
) {
    if findings_by_path.is_empty() {
        return;
    }
    let findings = findings_by_path
        .iter()
        .flat_map(|(path, identities)| {
            identities
                .iter()
                .map(move |identity| format!("  - {path}: {identity}"))
        })
        .collect::<Vec<_>>()
        .join("\n");
    output.push(format!("{title}:\n{findings}\n{guidance}"));
}

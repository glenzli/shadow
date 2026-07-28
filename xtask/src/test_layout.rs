use std::{env, ffi::OsString, io, path::PathBuf};

mod audit;
mod policy;

use audit::{TestLayoutObservation, audit_workspace, finding_count};

const OBSERVATION_SCHEMA: u32 = 2;

#[derive(Debug, Default)]
struct Options {
    help: bool,
    repository_root: Option<PathBuf>,
    print_observed: bool,
    verbose: bool,
}

impl Options {
    fn parse(arguments: impl IntoIterator<Item = OsString>) -> io::Result<Self> {
        let mut options = Self::default();
        let mut arguments = arguments.into_iter();
        while let Some(argument) = arguments.next() {
            match argument.to_string_lossy().as_ref() {
                "--root" => {
                    options.repository_root =
                        Some(PathBuf::from(arguments.next().ok_or_else(|| {
                            invalid_argument("test-layout --root requires a path")
                        })?));
                }
                "--print-observed" => options.print_observed = true,
                "--verbose" => options.verbose = true,
                "--help" | "-h" => {
                    println!(
                        "usage: cargo xtask test-layout [--root PATH] [--verbose] [--print-observed]"
                    );
                    options.help = true;
                    return Ok(options);
                }
                other => {
                    return Err(invalid_argument(format!(
                        "unknown test-layout argument: {other}"
                    )));
                }
            }
        }
        Ok(options)
    }
}

pub fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let options = Options::parse(arguments)?;
    if options.help {
        return Ok(());
    }
    let repository_root = options.repository_root.unwrap_or_else(|| {
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .expect("xtask lives directly below the repository root")
            .to_path_buf()
    });
    let repository_root = repository_root.canonicalize()?;
    let observation = audit_workspace(&repository_root)?;
    policy::enforce(&observation)?;

    if options.print_observed {
        let observed = serde_json::json!({
            "schema": OBSERVATION_SCHEMA,
            "inline_test_modules": &observation.inline_test_modules,
            "inline_executable_tests": &observation.inline_executable_tests,
            "owner_test_path_overrides": &observation.owner_test_path_overrides,
            "crate_test_source_inclusions": &observation.crate_test_source_inclusions,
            "permanently_disabled_test_sources": &observation.permanently_disabled_test_sources,
            "test_facade_non_registration_items":
                &observation.test_facade_non_registration_items,
            "sibling_test_sources": &observation.sibling_test_sources,
        });
        println!(
            "{}",
            serde_json::to_string_pretty(&observed)
                .map_err(|error| invalid_data(error.to_string()))?
        );
        return Ok(());
    }

    let inline_module_count = finding_count(&observation.inline_test_modules);
    let inline_test_count = finding_count(&observation.inline_executable_tests);
    let path_override_count = finding_count(&observation.owner_test_path_overrides);
    let inclusion_count = finding_count(&observation.crate_test_source_inclusions);
    let disabled_count = finding_count(&observation.permanently_disabled_test_sources);
    let facade_item_count = finding_count(&observation.test_facade_non_registration_items);
    println!(
        "test-layout: {inline_module_count} inline modules / {inline_test_count} inline tests, \
         {path_override_count} owner test path overrides, {inclusion_count} source inclusions, \
         {disabled_count} permanently disabled test conditions, {facade_item_count} test-facade \
         implementation items; strict topology satisfied"
    );
    if options.verbose {
        print_verbose_observation(&observation);
    }
    Ok(())
}

fn print_verbose_observation(observation: &TestLayoutObservation) {
    for (path, identities) in &observation.inline_test_modules {
        println!("  {path}: {} inline test module(s)", identities.len());
    }
    for (path, identities) in &observation.inline_executable_tests {
        println!("  {path}: {}", identities.len());
    }
    for (path, identities) in &observation.owner_test_path_overrides {
        println!("  {path}: {} owner test path override(s)", identities.len());
    }
    for (path, identities) in &observation.crate_test_source_inclusions {
        println!("  {path}: {} source inclusion(s)", identities.len());
    }
    for (path, identities) in &observation.permanently_disabled_test_sources {
        println!(
            "  {path}: {} permanently disabled test condition(s)",
            identities.len()
        );
    }
    for (path, identities) in &observation.test_facade_non_registration_items {
        println!(
            "  {path}: {} non-registration test-facade item(s)",
            identities.len()
        );
    }
}

fn invalid_argument(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

fn invalid_data(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.into())
}

#[cfg(test)]
mod tests;

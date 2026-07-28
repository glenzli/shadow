use super::{
    fixtures::{
        collect_supported_raws, first_supported_raw, materialize_raw_probe,
        resolve_raw_fixture_directory,
    },
    process::run_checked,
};
use std::{env, fs, io, path::Path, process::Command, time::Instant};

/// Enables the slower, hardware-local verification tier without making the
/// normal edit-loop smoke expensive.
pub(super) fn enabled() -> bool {
    matches!(env::var("SHADOW_DAILY_USE_EXTENDED").as_deref(), Ok("1"))
}

/// Runs the slower local baseline suite after the normal desktop workflow is
/// known-good. Each phase delegates its oracle to the package that owns it.
pub(super) fn run(
    repository_root: &Path,
    fixture_directory: &Path,
    data_root: &Path,
) -> io::Result<()> {
    let raw_directory = resolve_raw_fixture_directory(repository_root)?;
    let export_fixture = first_supported_raw(fixture_directory)
        .or_else(|| first_supported_raw(&raw_directory))
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "extended baseline could not find a supported RAW/DNG export fixture",
            )
        })?;
    let cargo_target = data_root.join("extended-cargo-target");

    println!(
        "daily-use extended baseline: raw-fixtures={} export-fixture={}",
        raw_directory.display(),
        export_fixture.display()
    );
    let mut failures = Vec::new();
    record_phase(&mut failures, "mixed-vendor RAW capability matrix", || {
        run_raw_fixture_matrix(repository_root, &raw_directory, &cargo_target, data_root)
    });
    record_phase(&mut failures, "full-resolution export raster", || {
        run_checked(
            Command::new("cargo")
                .current_dir(repository_root)
                .env("CARGO_TARGET_DIR", &cargo_target)
                .env("SHADOW_TEST_EXPORT_RAW", &export_fixture)
                .args([
                    "test",
                    "-p",
                    "shadow-desktop-bridge",
                    "--lib",
                    "real_raw_export_renders_a_tightly_packed_full_resolution_raster",
                    "--",
                    "--ignored",
                    "--nocapture",
                ]),
            "full-resolution export raster",
        )
    });
    record_phase(
        &mut failures,
        "preview cache integrity and reclamation",
        || {
            run_checked(
                Command::new("cargo")
                    .current_dir(repository_root)
                    .env("CARGO_TARGET_DIR", &cargo_target)
                    .args(["test", "-p", "shadow-cache", "--lib"]),
                "preview cache integrity and reclamation",
            )
        },
    );
    record_phase(&mut failures, "cached artifact identity validity", || {
        run_checked(
            Command::new("cargo")
                .current_dir(repository_root)
                .env("CARGO_TARGET_DIR", &cargo_target)
                .args([
                    "test",
                    "-p",
                    "shadow-catalog",
                    "--lib",
                    "cache_artifact::tests",
                ]),
            "cached artifact identity validity",
        )
    });
    record_phase(
        &mut failures,
        "durable export queue, receipt, and recovery",
        || {
            run_checked(
                Command::new("cargo")
                    .current_dir(repository_root)
                    .env("CARGO_TARGET_DIR", &cargo_target)
                    .args([
                        "test",
                        "-p",
                        "shadow-catalog",
                        "--lib",
                        "export_queue::tests",
                    ]),
                "durable export queue, receipt, and recovery",
            )
        },
    );
    if failures.is_empty() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{} extended automation baseline phase(s) failed:\n{}",
            failures.len(),
            failures.join("\n\n")
        )))
    }
}

fn record_phase(
    failures: &mut Vec<String>,
    context: &str,
    operation: impl FnOnce() -> io::Result<()>,
) {
    println!("daily-use extended baseline: {context}");
    let started = Instant::now();
    match operation() {
        Ok(()) => println!(
            "daily-use extended baseline passed: {context} ({:.2}s)",
            started.elapsed().as_secs_f64()
        ),
        Err(error) => {
            println!(
                "daily-use extended baseline failed: {context} ({:.2}s)",
                started.elapsed().as_secs_f64()
            );
            failures.push(format!("{context}: {error}"));
        }
    }
}

/// Runs every local RAW in a separate process so one native decoder crash
/// cannot hide the identity of the remaining fixtures.
fn run_raw_fixture_matrix(
    repository_root: &Path,
    raw_directory: &Path,
    cargo_target: &Path,
    data_root: &Path,
) -> io::Result<()> {
    let mut sources = Vec::new();
    collect_supported_raws(raw_directory, &mut sources);
    sources.sort();
    if sources.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "RAW fixture directory contains no supported files: {}",
                raw_directory.display()
            ),
        ));
    }

    let probe_root = data_root.join("raw-matrix-probes");
    fs::create_dir_all(&probe_root)?;
    let mut failures = Vec::new();
    for (index, source) in sources.iter().enumerate() {
        let probe_directory = probe_root.join(format!("{index:03}"));
        fs::create_dir(&probe_directory)?;
        let probe_path = probe_directory.join(
            source
                .file_name()
                .ok_or_else(|| io::Error::other("RAW fixture has no file name"))?,
        );
        materialize_raw_probe(source, &probe_path)?;
        let context = format!("RAW probe {}", source.display());
        match run_checked(
            Command::new("cargo")
                .current_dir(repository_root)
                .env("CARGO_TARGET_DIR", cargo_target)
                .env("SHADOW_TEST_RAW_FOLDER", &probe_directory)
                .args([
                    "test",
                    "-p",
                    "shadow-bridge",
                    "--lib",
                    "real_raw_folder_smoke_matrix",
                    "--",
                    "--ignored",
                    "--nocapture",
                ]),
            &context,
        ) {
            Ok(()) => println!("daily-use RAW baseline passed: {}", source.display()),
            Err(error) => failures.push(format!("{}: {error}", source.display())),
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{} of {} RAW fixtures failed without aborting the matrix:\n{}",
            failures.len(),
            sources.len(),
            failures.join("\n")
        )))
    }
}

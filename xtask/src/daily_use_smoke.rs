//! End-to-end acceptance smoke for Shadow's normal local workflow.
//!
//! This module deliberately orchestrates the desktop application's existing
//! headless smoke switches instead of duplicating controller behavior in Rust.
//! Every phase shares one short-lived Catalog/cache root, so persistence between
//! launches is part of the contract being tested.

use std::{
    env,
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
    process::{Command, Output},
    time::{Instant, SystemTime, UNIX_EPOCH},
};

const DEFAULT_FIXTURE_DIRECTORY: &str = "local-reference/sample-assets/dng";
const DEFAULT_RAW_FIXTURE_DIRECTORY: &str = "local-reference/sample-assets/raw";
const DESKTOP_APP_EXECUTABLE: &str = "apps/desktop/Shadow.app/Contents/MacOS/Shadow";

pub fn run(fixture_argument: Option<OsString>) -> io::Result<()> {
    let repository_root = repository_root();
    let fixture_directory = resolve_fixture_directory(&repository_root, fixture_argument)?;
    let extended = extended_baselines_enabled();

    build_release_desktop(&repository_root)?;
    let executable = release_desktop_executable(&repository_root)?;
    if !executable.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "desktop build completed but the app executable is missing: {}",
                executable.display()
            ),
        ));
    }

    let session = AcceptanceSession::create()?;
    println!(
        "daily-use smoke: fixture={} data-root={}",
        fixture_directory.display(),
        session.data_root.display()
    );

    let result = run_scenario(&executable, &fixture_directory, &session.data_root).and_then(|()| {
        if extended {
            run_extended_baselines(&repository_root, &fixture_directory, &session.data_root)
        } else {
            Ok(())
        }
    });
    match result {
        Ok(()) => {
            session.remove()?;
            println!("daily-use smoke passed");
            Ok(())
        }
        Err(error) => {
            eprintln!(
                "daily-use smoke failed; preserving its Catalog/cache for inspection: {}",
                session.data_root.display()
            );
            Err(error)
        }
    }
}

/// Enables the slower, hardware-local verification tier without making the
/// normal edit-loop smoke expensive.  It deliberately requires local fixtures
/// and runs isolated Cargo artifacts, so it is suited to a developer's Mac,
/// not a portable CI worker.
fn extended_baselines_enabled() -> bool {
    matches!(env::var("SHADOW_DAILY_USE_EXTENDED").as_deref(), Ok("1"))
}

fn run_scenario(executable: &Path, fixture_directory: &Path, data_root: &Path) -> io::Result<()> {
    run_phase(
        "streaming import",
        executable,
        data_root,
        Some(fixture_directory),
        &["SHADOW_DESKTOP_STREAMING_SCAN_SMOKE"],
    )?;
    run_phase(
        "persisted library reopen",
        executable,
        data_root,
        None,
        &["SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE"],
    )?;
    run_phase(
        "edit preview and neutral baseline",
        executable,
        data_root,
        None,
        &[
            "SHADOW_DESKTOP_OPEN_FIRST_EDIT",
            "SHADOW_DESKTOP_REQUEST_BEFORE",
        ],
    )?;
    run_phase(
        "non-destructive grade stack and named version",
        executable,
        data_root,
        None,
        &[
            "SHADOW_DESKTOP_OPEN_FIRST_EDIT",
            "SHADOW_DESKTOP_GRADE_STACK_SMOKE",
        ],
    )?;
    run_phase(
        "pick decision and undo ledger",
        executable,
        data_root,
        None,
        &[
            "SHADOW_DESKTOP_SET_FIRST_DECISION",
            "SHADOW_DESKTOP_UNDO_FIRST_DECISION",
        ],
    )?;
    run_phase(
        "autosave on close",
        executable,
        data_root,
        None,
        &[
            "SHADOW_DESKTOP_OPEN_FIRST_EDIT",
            "SHADOW_DESKTOP_DIRTY_CLOSE_SMOKE",
        ],
    )
}

/// Runs the slower local baseline suite after the normal desktop workflow is
/// known-good.  Every phase has one narrow, durable responsibility:
///
/// - mixed-vendor RAW capability and bounded preview rendering;
/// - a representative full-resolution export raster;
/// - cache corruption/invalidation safety; and
/// - durable export queue and receipt/recovery semantics.
///
/// The checks intentionally reuse the owning packages' contract tests rather
/// than reproducing behavior in `xtask`.  This keeps the test oracle beside
/// the code that owns the invariant.
fn run_extended_baselines(
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
    record_extended_phase(&mut failures, "mixed-vendor RAW capability matrix", || {
        run_raw_fixture_matrix(repository_root, &raw_directory, &cargo_target, data_root)
    });
    record_extended_phase(&mut failures, "full-resolution export raster", || {
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
    record_extended_phase(
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
    record_extended_phase(&mut failures, "cached artifact identity validity", || {
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
    record_extended_phase(
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

fn record_extended_phase(
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

/// Runs every local RAW in a separate process.  Native decoders occasionally
/// encounter malformed or newly introduced proprietary payloads; a baseline
/// must report the exact path instead of letting one native crash terminate the
/// entire diagnostic matrix.
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

fn materialize_raw_probe(source: &Path, destination: &Path) -> io::Result<()> {
    #[cfg(unix)]
    {
        if std::os::unix::fs::symlink(source, destination).is_ok() {
            return Ok(());
        }
    }
    #[cfg(windows)]
    {
        if std::os::windows::fs::symlink_file(source, destination).is_ok() {
            return Ok(());
        }
    }
    if fs::hard_link(source, destination).is_ok() {
        return Ok(());
    }
    fs::copy(source, destination).map(|_| ())
}

fn resolve_raw_fixture_directory(repository_root: &Path) -> io::Result<PathBuf> {
    let raw_directory = env::var_os("SHADOW_BASELINE_RAW_FIXTURE_DIR").map_or_else(
        || repository_root.join(DEFAULT_RAW_FIXTURE_DIRECTORY),
        |value| {
            let path = PathBuf::from(value);
            if path.is_absolute() {
                path
            } else {
                repository_root.join(path)
            }
        },
    );
    if !raw_directory.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "extended RAW fixture directory does not exist: {}",
                raw_directory.display()
            ),
        ));
    }
    raw_directory.canonicalize()
}

fn first_supported_raw(directory: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    collect_supported_raws(directory, &mut candidates);
    candidates.sort();
    candidates.into_iter().next()
}

fn collect_supported_raws(directory: &Path, paths: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(directory) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_supported_raws(&path, paths);
        } else if path
            .extension()
            .and_then(|extension| extension.to_str())
            .is_some_and(is_supported_raw_extension)
        {
            paths.push(path);
        }
    }
}

fn is_supported_raw_extension(extension: &str) -> bool {
    matches!(
        extension.to_ascii_lowercase().as_str(),
        "3fr"
            | "arw"
            | "cr2"
            | "cr3"
            | "dng"
            | "erf"
            | "fff"
            | "iiq"
            | "kdc"
            | "mef"
            | "mos"
            | "mrw"
            | "nef"
            | "nrw"
            | "orf"
            | "pef"
            | "raf"
            | "raw"
            | "rw2"
            | "rwl"
            | "sr2"
            | "srf"
            | "srw"
    )
}

fn build_release_desktop(repository_root: &Path) -> io::Result<()> {
    if matches!(
        env::var("SHADOW_DAILY_USE_SKIP_DESKTOP_BUILD").as_deref(),
        Ok("1")
    ) {
        println!("daily-use smoke: reusing the existing optimized desktop bundle");
        return Ok(());
    }
    println!("daily-use smoke: configuring optimized desktop bundle");
    run_checked(
        Command::new("cmake")
            .current_dir(repository_root)
            .args(["--preset", "desktop-release"]),
        "desktop release configuration",
    )?;
    println!("daily-use smoke: building optimized desktop bundle");
    run_checked(
        Command::new("cmake").current_dir(repository_root).args([
            "--build",
            "--preset",
            "desktop-release",
        ]),
        "desktop release build",
    )
}

fn run_phase(
    phase: &str,
    executable: &Path,
    data_root: &Path,
    scan_directory: Option<&Path>,
    switches: &[&str],
) -> io::Result<()> {
    println!("daily-use smoke: {phase}");
    let mut command = Command::new(executable);
    command
        .env("QT_QPA_PLATFORM", "offscreen")
        .env("SHADOW_DESKTOP_SMOKE_TEST", "1")
        .env("SHADOW_DESKTOP_DATA_ROOT", data_root)
        .env_remove("SHADOW_DESKTOP_SCAN_FOLDER");
    if let Some(scan_directory) = scan_directory {
        command.env("SHADOW_DESKTOP_SCAN_FOLDER", scan_directory);
    }
    for switch in switches {
        command.env(switch, "1");
    }
    run_checked(&mut command, phase)
}

fn resolve_fixture_directory(
    repository_root: &Path,
    fixture_argument: Option<OsString>,
) -> io::Result<PathBuf> {
    let fixture_directory = fixture_argument.map_or_else(
        || repository_root.join(DEFAULT_FIXTURE_DIRECTORY),
        |argument| {
            let path = PathBuf::from(argument);
            if path.is_absolute() {
                path
            } else {
                repository_root.join(path)
            }
        },
    );
    if !fixture_directory.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "daily-use fixture directory does not exist: {}",
                fixture_directory.display()
            ),
        ));
    }
    fixture_directory.canonicalize()
}

fn repository_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf()
}

fn release_desktop_executable(repository_root: &Path) -> io::Result<PathBuf> {
    let parent = repository_root.parent().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "Shadow repository has no parent directory for local build artifacts",
        )
    })?;
    Ok(parent
        .join(".shadow-local-build/desktop-release")
        .join(DESKTOP_APP_EXECUTABLE))
}

fn run_checked(command: &mut Command, context: &str) -> io::Result<()> {
    let output = command.output()?;
    if output.status.success() {
        return Ok(());
    }
    Err(io::Error::other(format!(
        "{context} exited with status {}\n{}",
        output.status,
        output_summary(&output)
    )))
}

fn output_summary(output: &Output) -> String {
    const MAX_BYTES: usize = 8 * 1024;
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    let combined = format!("stderr:\n{stderr}\nstdout:\n{stdout}");
    if combined.len() <= MAX_BYTES {
        combined
    } else {
        format!("…{}", utf8_tail(&combined, MAX_BYTES))
    }
}

fn utf8_tail(value: &str, maximum_bytes: usize) -> &str {
    let mut start = value.len().saturating_sub(maximum_bytes);
    while start < value.len() && !value.is_char_boundary(start) {
        start += 1;
    }
    &value[start..]
}

struct AcceptanceSession {
    data_root: PathBuf,
}

impl AcceptanceSession {
    fn create() -> io::Result<Self> {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(io::Error::other)?
            .as_millis();
        let data_root = env::temp_dir().join(format!(
            "shadow-daily-use-{}-{timestamp}",
            std::process::id()
        ));
        fs::create_dir_all(&data_root)?;
        Ok(Self { data_root })
    }

    fn remove(self) -> io::Result<()> {
        fs::remove_dir_all(self.data_root)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf8_tail_starts_at_a_character_boundary() {
        assert_eq!(utf8_tail("ab中文", 4), "文");
    }

    #[test]
    fn optimized_desktop_bundle_is_outside_the_shared_source_root() {
        let root = Path::new("/tmp/shadow-workspace");
        assert_eq!(
            release_desktop_executable(root).expect("derive desktop app path"),
            PathBuf::from(
                "/tmp/.shadow-local-build/desktop-release/apps/desktop/Shadow.app/Contents/MacOS/Shadow"
            )
        );
    }
}

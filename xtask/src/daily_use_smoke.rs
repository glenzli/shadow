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
    time::{SystemTime, UNIX_EPOCH},
};

const DEFAULT_FIXTURE_DIRECTORY: &str = "local-reference/sample-assets/dng";
const DESKTOP_EXECUTABLE: &str =
    "build/desktop-release/apps/desktop/Shadow.app/Contents/MacOS/Shadow";

pub fn run(fixture_argument: Option<OsString>) -> io::Result<()> {
    let repository_root = repository_root();
    let fixture_directory = resolve_fixture_directory(&repository_root, fixture_argument)?;

    build_release_desktop(&repository_root)?;
    let executable = repository_root.join(DESKTOP_EXECUTABLE);
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

    let result = run_scenario(&executable, &fixture_directory, &session.data_root);
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

fn build_release_desktop(repository_root: &Path) -> io::Result<()> {
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
}

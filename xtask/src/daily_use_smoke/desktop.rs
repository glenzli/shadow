use super::process::{run_checked, run_desktop_checked};
use std::{
    env, io,
    path::{Path, PathBuf},
    process::Command,
};

const DESKTOP_APP_EXECUTABLE: &str = "apps/desktop/Shadow.app/Contents/MacOS/Shadow";

pub(super) fn run_scenario(
    executable: &Path,
    fixture_directory: &Path,
    data_root: &Path,
) -> io::Result<()> {
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

pub(super) fn build_release(repository_root: &Path) -> io::Result<()> {
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

pub(super) fn release_executable(repository_root: &Path) -> io::Result<PathBuf> {
    release_executable_from(
        repository_root,
        env::var_os("SHADOW_DAILY_USE_DESKTOP_EXECUTABLE"),
    )
}

fn release_executable_from(
    repository_root: &Path,
    explicit_executable: Option<std::ffi::OsString>,
) -> io::Result<PathBuf> {
    if let Some(explicit_executable) = explicit_executable {
        let path = PathBuf::from(explicit_executable);
        if !path.is_absolute() || path.starts_with(repository_root) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "SHADOW_DAILY_USE_DESKTOP_EXECUTABLE must be an absolute path outside {}; received {}",
                    repository_root.display(),
                    path.display()
                ),
            ));
        }
        return Ok(path);
    }
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
    run_desktop_checked(&mut command, phase)
}

#[cfg(test)]
mod tests;

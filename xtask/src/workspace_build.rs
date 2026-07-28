use std::{
    env, io,
    path::{Path, PathBuf},
    process::Command,
};

pub(super) fn run(program: &str, arguments: &[&str]) -> io::Result<()> {
    let status = Command::new(program).args(arguments).status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{program} exited with status {status}"
        )))
    }
}

pub(super) fn configure_preset(preset: &str) -> io::Result<PathBuf> {
    let build_directory = preset_build_directory(preset)?;
    let status = Command::new("cmake")
        .args(["--preset", preset, "-B"])
        .arg(&build_directory)
        .status()?;
    if status.success() {
        Ok(build_directory)
    } else {
        Err(io::Error::other(format!(
            "cmake configure preset {preset} exited with status {status}"
        )))
    }
}

pub(super) fn build_preset(preset: &str) -> io::Result<()> {
    let build_directory = preset_build_directory(preset)?;
    build_directory_contents(&build_directory)
}

pub(super) fn build_directory_contents(build_directory: &Path) -> io::Result<()> {
    let status = Command::new("cmake")
        .args(["--build"])
        .arg(build_directory)
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "cmake build {} exited with status {status}",
            build_directory.display()
        )))
    }
}

pub(super) fn run_ctest(build_directory: &Path) -> io::Result<()> {
    let status = Command::new("ctest")
        .args(["--test-dir"])
        .arg(build_directory)
        .arg("--output-on-failure")
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "ctest for {} exited with status {status}",
            build_directory.display()
        )))
    }
}

fn preset_build_directory(preset: &str) -> io::Result<PathBuf> {
    let repository_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .canonicalize()?;
    let build_directory = env::var_os("SHADOW_BUILD_DIR").map_or_else(
        || {
            repository_root
                .parent()
                .expect("repository root has a parent directory")
                .join(".shadow-local-build")
                .join(preset)
        },
        PathBuf::from,
    );
    if !build_directory.is_absolute() || build_directory.starts_with(&repository_root) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "SHADOW_BUILD_DIR must be an absolute path outside {}; received {}",
                repository_root.display(),
                build_directory.display()
            ),
        ));
    }
    Ok(build_directory)
}

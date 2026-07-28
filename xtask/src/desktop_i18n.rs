use std::{
    env, io,
    path::{Path, PathBuf},
    process::{Command, ExitStatus},
};

pub(crate) fn run() -> io::Result<()> {
    let repository_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf();
    let checker = repository_root.join("scripts/check_qt_translations.py");
    let python = env::var_os("PYTHON").unwrap_or_else(|| "python3".into());
    let status = Command::new(&python)
        .current_dir(&repository_root)
        .arg(&checker)
        .status()
        .map_err(|error| {
            io::Error::new(
                error.kind(),
                format!(
                    "launch desktop translation checker {} with {:?}: {error}",
                    checker.display(),
                    Path::new(&python).display()
                ),
            )
        })?;
    require_success(status)
}

fn require_success(status: ExitStatus) -> io::Result<()> {
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "desktop translation contract exited with status {status}"
        )))
    }
}

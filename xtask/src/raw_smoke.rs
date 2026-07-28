use std::{ffi::OsString, io, path::PathBuf, process::Command};

pub(super) fn run(folder: Option<OsString>) -> io::Result<()> {
    let repository_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf();
    let folder = folder.map_or_else(
        || repository_root.join("local-reference/sample-assets/raw"),
        |path| {
            let path = PathBuf::from(path);
            if path.is_absolute() {
                path
            } else {
                repository_root.join(path)
            }
        },
    );
    if !folder.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("RAW fixture directory does not exist: {}", folder.display()),
        ));
    }
    let folder = folder.canonicalize()?;

    let status = Command::new("cargo")
        .current_dir(repository_root)
        .env("SHADOW_TEST_RAW_FOLDER", &folder)
        .args([
            "test",
            "-p",
            "shadow-bridge",
            "real_raw_folder_smoke_matrix",
            "--",
            "--ignored",
            "--nocapture",
        ])
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "RAW smoke matrix exited with status {status}"
        )))
    }
}

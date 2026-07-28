use std::{
    collections::BTreeSet,
    env,
    ffi::OsString,
    io,
    path::{Path, PathBuf},
    process::{Command, Output},
};

pub(super) fn run(arguments: impl Iterator<Item = OsString>) -> io::Result<()> {
    let root = repository_root()?;
    let mut check = false;
    let mut all = false;
    let mut requested = Vec::new();
    for argument in arguments {
        match argument.to_str() {
            Some("--check") => check = true,
            Some("--all") => all = true,
            Some(value) if value.starts_with('-') => {
                return Err(invalid_argument(format!(
                    "unknown format argument {value:?}"
                )));
            }
            _ => requested.push(PathBuf::from(argument)),
        }
    }
    if all && !requested.is_empty() {
        return Err(invalid_argument(
            "format --all cannot be combined with explicit paths",
        ));
    }

    run_rustfmt(&root, check)?;
    let native_paths = if all {
        tracked_native_paths(&root)?
    } else if requested.is_empty() {
        changed_native_paths(&root)?
    } else {
        validate_requested_native_paths(&root, requested)?
    };
    run_clang_format(&root, check, native_paths)
}

fn repository_root() -> io::Result<PathBuf> {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .canonicalize()
}

fn run_rustfmt(root: &Path, check: bool) -> io::Result<()> {
    let mut command = Command::new("cargo");
    command.current_dir(root).arg("fmt");
    if check {
        command.arg("--check");
    }
    run_status(command, "cargo fmt")
}

fn tracked_native_paths(root: &Path) -> io::Result<BTreeSet<PathBuf>> {
    let output = Command::new("git")
        .current_dir(root)
        .args(["ls-files", "-z"])
        .output()?;
    native_paths_from_git_output(root, output, "git ls-files")
}

fn changed_native_paths(root: &Path) -> io::Result<BTreeSet<PathBuf>> {
    let tracked = Command::new("git")
        .current_dir(root)
        .args(["diff", "--name-only", "--diff-filter=ACMR", "-z"])
        .output()?;
    let untracked = Command::new("git")
        .current_dir(root)
        .args(["ls-files", "--others", "--exclude-standard", "-z"])
        .output()?;
    let mut paths = native_paths_from_git_output(root, tracked, "git diff")?;
    paths.extend(native_paths_from_git_output(
        root,
        untracked,
        "git ls-files --others",
    )?);
    Ok(paths)
}

fn native_paths_from_git_output(
    root: &Path,
    output: Output,
    label: &str,
) -> io::Result<BTreeSet<PathBuf>> {
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "{label} exited with status {}",
            output.status
        )));
    }
    let text = String::from_utf8(output.stdout)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    Ok(text
        .split('\0')
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .filter(|path| is_native_source(path))
        .map(|path| root.join(path))
        .collect())
}

fn validate_requested_native_paths(
    root: &Path,
    requested: Vec<PathBuf>,
) -> io::Result<BTreeSet<PathBuf>> {
    let mut paths = BTreeSet::new();
    for requested_path in requested {
        let joined = if requested_path.is_absolute() {
            requested_path
        } else {
            root.join(requested_path)
        };
        let canonical = joined.canonicalize().map_err(|error| {
            io::Error::new(
                error.kind(),
                format!("cannot resolve format path {}: {error}", joined.display()),
            )
        })?;
        if !canonical.starts_with(root) || !canonical.is_file() {
            return Err(invalid_argument(format!(
                "format path must be a file inside {}: {}",
                root.display(),
                canonical.display()
            )));
        }
        if !is_native_source(&canonical) {
            return Err(invalid_argument(format!(
                "format path is not a supported native source: {}",
                canonical.display()
            )));
        }
        paths.insert(canonical);
    }
    Ok(paths)
}

fn is_native_source(path: &Path) -> bool {
    matches!(
        path.extension().and_then(|value| value.to_str()),
        Some("c" | "cc" | "cpp" | "cxx" | "h" | "hh" | "hpp" | "hxx" | "m" | "mm")
    )
}

fn run_clang_format(root: &Path, check: bool, paths: BTreeSet<PathBuf>) -> io::Result<()> {
    if paths.is_empty() {
        println!("format: no changed native sources");
        return Ok(());
    }
    let mut command = Command::new("xcrun");
    command.current_dir(root).arg("clang-format");
    if check {
        command.args(["--dry-run", "--Werror"]);
    } else {
        command.arg("-i");
    }
    command.args(paths);
    run_status(command, "clang-format")
}

fn run_status(mut command: Command, label: &str) -> io::Result<()> {
    let status = command.status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{label} exited with status {status}"
        )))
    }
}

fn invalid_argument(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

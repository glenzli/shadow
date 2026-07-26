//! Reject generated local payloads in a same-machine shared source worktree.
//!
//! Git ignores are intentionally not used as a transport boundary: a workspace
//! synchronizer can upload ignored files. This guard is therefore independent
//! of Git and checks the concrete high-volume paths that must stay outside the
//! repository in the `local-shared-workspace` collaboration topology.

use std::{
    env,
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
};

const GUARDED_PATHS: &[&str] = &[
    "target",
    "build",
    "local-reference/sample-assets",
    "local-reference/experiment-output",
];

#[derive(Debug, Default)]
struct GuardArguments {
    root: Option<PathBuf>,
}

impl GuardArguments {
    fn parse(arguments: impl IntoIterator<Item = OsString>) -> io::Result<Self> {
        let mut arguments = arguments.into_iter();
        let mut parsed = Self::default();
        while let Some(argument) = arguments.next() {
            match argument.to_string_lossy().as_ref() {
                "--root" => {
                    let value = arguments.next().ok_or_else(|| {
                        invalid_argument("local-workspace-guard --root requires a path")
                    })?;
                    parsed.root = Some(PathBuf::from(value));
                }
                "--help" | "-h" => {
                    return Err(invalid_argument(
                        "usage: cargo xtask local-workspace-guard [--root PATH]",
                    ));
                }
                other => {
                    return Err(invalid_argument(&format!(
                        "unknown local-workspace-guard argument: {other}",
                    )));
                }
            }
        }
        Ok(parsed)
    }
}

pub fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let arguments = GuardArguments::parse(arguments)?;
    let root = arguments.root.unwrap_or_else(repository_root);
    let root = root.canonicalize()?;
    let mut violations = Vec::new();

    for relative_path in GUARDED_PATHS {
        let path = root.join(relative_path);
        match guarded_path_size(&root, &path)? {
            GuardedPathSize::Bytes(bytes) if bytes > 0 => {
                violations.push(format!("{} ({})", path.display(), format_bytes(bytes)));
            }
            GuardedPathSize::ExternalDirectoryLink(target) => {
                println!(
                    "local workspace guard: accepting external local payload link {} -> {}",
                    path.display(),
                    target.display()
                );
            }
            GuardedPathSize::Bytes(_) => {}
        }
    }
    for variable in ["CARGO_TARGET_DIR", "SHADOW_BUILD_DIR"] {
        if let Some(value) = env::var_os(variable) {
            let path = PathBuf::from(value);
            if !path.is_absolute() || path.starts_with(&root) {
                violations.push(format!(
                    "{variable}={} must be an absolute path outside {}",
                    path.display(),
                    root.display()
                ));
            }
        }
    }

    if violations.is_empty() {
        println!(
            "local workspace guard: passed; no generated payload is present below {}",
            root.display()
        );
        return Ok(());
    }

    eprintln!(
        "local workspace guard: blocked; this same-machine collaboration must not sync generated payloads from the source root:"
    );
    for violation in violations {
        eprintln!("  - {violation}");
    }
    Err(io::Error::other(
        "move or safely remove the listed local payloads after their owners release them, then use an external CARGO_TARGET_DIR and SHADOW_BUILD_DIR",
    ))
}

fn repository_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf()
}

enum GuardedPathSize {
    Bytes(u64),
    ExternalDirectoryLink(PathBuf),
}

fn guarded_path_size(root: &Path, path: &Path) -> io::Result<GuardedPathSize> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            return Ok(GuardedPathSize::Bytes(0));
        }
        Err(error) => return Err(error),
    };
    if metadata.file_type().is_symlink() {
        let configured_target = fs::read_link(path)?;
        if !configured_target.is_absolute() {
            return Err(io::Error::other(format!(
                "guarded payload link {} must use an absolute external target",
                path.display()
            )));
        }
        let canonical_target = fs::canonicalize(path).map_err(|error| {
            io::Error::new(
                error.kind(),
                format!(
                    "guarded payload link {} must resolve to an existing external directory: {error}",
                    path.display()
                ),
            )
        })?;
        if !canonical_target.is_dir() || canonical_target.starts_with(root) {
            return Err(io::Error::other(format!(
                "guarded payload link {} must resolve to a directory outside {}",
                path.display(),
                root.display()
            )));
        }
        return Ok(GuardedPathSize::ExternalDirectoryLink(canonical_target));
    }
    if !metadata.is_dir() {
        return Ok(GuardedPathSize::Bytes(metadata.len()));
    }

    let mut total = 0_u64;
    let mut pending = vec![path.to_path_buf()];
    while let Some(directory) = pending.pop() {
        for entry in fs::read_dir(directory)? {
            let entry = entry?;
            let metadata = fs::symlink_metadata(entry.path())?;
            if metadata.is_dir() {
                pending.push(entry.path());
            } else if metadata.is_file() {
                total = total.saturating_add(metadata.len());
            }
        }
    }
    Ok(GuardedPathSize::Bytes(total))
}

fn format_bytes(bytes: u64) -> String {
    const GIB: u64 = 1024 * 1024 * 1024;
    const MIB: u64 = 1024 * 1024;
    if bytes >= GIB {
        format_decimal_size(bytes, GIB, "GiB")
    } else if bytes >= MIB {
        format_decimal_size(bytes, MIB, "MiB")
    } else {
        format!("{bytes} B")
    }
}

fn format_decimal_size(bytes: u64, unit: u64, suffix: &str) -> String {
    let whole = bytes / unit;
    let tenths = (bytes % unit).saturating_mul(10) / unit;
    format!("{whole}.{tenths} {suffix}")
}

fn invalid_argument(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

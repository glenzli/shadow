use std::{
    collections::BTreeSet,
    env,
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
    time::SystemTime,
};

use crate::desktop_workflow;

const DEFAULT_KEEP_DEBUG_RELEASES: usize = 3;

pub(crate) fn report() -> io::Result<()> {
    let paths = StoragePaths::resolve()?;
    let releases = discover_releases(&paths.debug_release_root)?;
    let release_bytes = releases.iter().map(|release| release.bytes).sum::<u64>();
    let canonical_build_bytes = directory_bytes(&paths.canonical_build_directory)?;
    let cargo_target_bytes = directory_bytes(&paths.cargo_target_root)?;

    println!("local storage report");
    println!(
        "debug releases: {} across {} release(s)",
        format_bytes(release_bytes),
        releases.len()
    );
    println!(
        "canonical CMake build: {}",
        format_bytes(canonical_build_bytes)
    );
    println!("Cargo target root: {}", format_bytes(cargo_target_bytes));
    match current_release_name(&paths) {
        Ok(name) => println!("current debug release: {name}"),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            println!("current debug release: absent")
        }
        Err(error) => return Err(error),
    }
    println!(
        "debug-release cleanup: cargo xtask storage-prune-debug --keep {DEFAULT_KEEP_DEBUG_RELEASES}"
    );
    Ok(())
}

pub(crate) fn prune_debug(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let request = parse_prune_request(arguments)?;
    if request == PruneRequest::Help {
        print_prune_usage();
        return Ok(());
    }
    let PruneRequest::Run { keep, apply } = request else {
        unreachable!("help returned above");
    };

    let _lock = desktop_workflow::acquire_canonical_debug_lock()?;
    let paths = StoragePaths::resolve()?;
    let current = current_release_name(&paths)?;
    let releases = discover_releases(&paths.debug_release_root)?;
    let candidates = prune_candidates(releases, &current, keep);
    let reclaim_bytes = candidates.iter().map(|release| release.bytes).sum::<u64>();

    println!("current debug release: {current}");
    println!("retention: keep {keep} release(s), including current-debug");
    if candidates.is_empty() {
        println!("no obsolete debug releases to remove");
        return Ok(());
    }
    println!(
        "{} obsolete release(s), {} reclaimable:",
        candidates.len(),
        format_bytes(reclaim_bytes)
    );
    for candidate in &candidates {
        println!("  {} ({})", candidate.name, format_bytes(candidate.bytes));
    }
    if !apply {
        println!("dry run only; close any old debug apps, then rerun with --apply to delete them");
        return Ok(());
    }

    remove_candidates(&paths, &current, &candidates)?;
    println!("removed {} obsolete debug release(s)", candidates.len());
    Ok(())
}

#[derive(Debug, Eq, PartialEq)]
enum PruneRequest {
    Help,
    Run { keep: usize, apply: bool },
}

fn parse_prune_request(arguments: impl IntoIterator<Item = OsString>) -> io::Result<PruneRequest> {
    let values = arguments.into_iter().collect::<Vec<_>>();
    let mut keep = DEFAULT_KEEP_DEBUG_RELEASES;
    let mut apply = false;
    let mut index = 0;
    while index < values.len() {
        match values[index].to_string_lossy().as_ref() {
            "--help" | "-h" if values.len() == 1 => return Ok(PruneRequest::Help),
            "--apply" if !apply => apply = true,
            "--apply" => return Err(invalid("--apply may appear only once")),
            "--keep" => {
                index += 1;
                let Some(value) = values.get(index) else {
                    return Err(invalid("--keep requires a positive integer"));
                };
                keep = value
                    .to_string_lossy()
                    .parse::<usize>()
                    .map_err(|_| invalid("--keep requires a positive integer"))?;
                if keep == 0 {
                    return Err(invalid("--keep requires a positive integer"));
                }
            }
            _ => {
                return Err(invalid(
                    "usage: cargo xtask storage-prune-debug [--keep N] [--apply]",
                ));
            }
        }
        index += 1;
    }
    Ok(PruneRequest::Run { keep, apply })
}

#[derive(Debug)]
struct StoragePaths {
    debug_release_root: PathBuf,
    current_debug_link: PathBuf,
    canonical_build_directory: PathBuf,
    cargo_target_root: PathBuf,
}

impl StoragePaths {
    fn resolve() -> io::Result<Self> {
        let (repository_root, local_build_root) =
            desktop_workflow::canonical_debug_storage_paths()?;
        let repository_parent = repository_root
            .parent()
            .ok_or_else(|| io::Error::other("repository root has no parent"))?;
        let cargo_target_root = env::var_os("CARGO_TARGET_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| repository_parent.join(".shadow-local-target"));
        Ok(Self {
            debug_release_root: local_build_root.join("releases/debug"),
            current_debug_link: local_build_root.join("current-debug"),
            canonical_build_directory: local_build_root.join("canonical-debug-build"),
            cargo_target_root,
        })
    }
}

#[derive(Debug, Eq, PartialEq)]
struct Release {
    name: String,
    path: PathBuf,
    modified: SystemTime,
    bytes: u64,
}

fn discover_releases(root: &Path) -> io::Result<Vec<Release>> {
    let entries = match fs::read_dir(root) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(error),
    };
    let mut releases = Vec::new();
    for entry in entries {
        let entry = entry?;
        let path = entry.path();
        let metadata = fs::symlink_metadata(&path)?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            continue;
        }
        let Some(name) = entry.file_name().to_str().map(ToOwned::to_owned) else {
            continue;
        };
        releases.push(Release {
            name,
            path: path.clone(),
            modified: metadata.modified()?,
            bytes: directory_bytes(&path)?,
        });
    }
    Ok(releases)
}

fn current_release_name(paths: &StoragePaths) -> io::Result<String> {
    let metadata = fs::symlink_metadata(&paths.current_debug_link)?;
    if !metadata.file_type().is_symlink() {
        return Err(io::Error::other(format!(
            "current debug entry is not a symlink: {}",
            paths.current_debug_link.display()
        )));
    }
    let target = fs::read_link(&paths.current_debug_link)?;
    let target = if target.is_absolute() {
        target
    } else {
        paths
            .current_debug_link
            .parent()
            .ok_or_else(|| io::Error::other("current debug link has no parent"))?
            .join(target)
    };
    let target = target.canonicalize()?;
    let release_root = paths.debug_release_root.canonicalize()?;
    if target.parent() != Some(release_root.as_path()) {
        return Err(io::Error::other(format!(
            "current debug entry does not resolve to a direct debug release: {}",
            target.display()
        )));
    }
    target
        .file_name()
        .and_then(|name| name.to_str())
        .map(ToOwned::to_owned)
        .ok_or_else(|| io::Error::other("current debug release has no UTF-8 directory name"))
}

fn prune_candidates(mut releases: Vec<Release>, current: &str, keep: usize) -> Vec<Release> {
    releases.sort_by(|left, right| {
        right
            .modified
            .cmp(&left.modified)
            .then_with(|| left.name.cmp(&right.name))
    });
    let mut retained = BTreeSet::from([current.to_owned()]);
    for release in &releases {
        if retained.len() >= keep {
            break;
        }
        retained.insert(release.name.clone());
    }
    let mut candidates = releases
        .into_iter()
        .filter(|release| !retained.contains(&release.name))
        .collect::<Vec<_>>();
    candidates.sort_by(|left, right| left.modified.cmp(&right.modified));
    candidates
}

fn remove_candidates(
    paths: &StoragePaths,
    current: &str,
    candidates: &[Release],
) -> io::Result<()> {
    if current_release_name(paths)? != current {
        return Err(io::Error::other(
            "current debug release changed while planning cleanup; refusing to delete",
        ));
    }
    for candidate in candidates {
        if candidate.name == current || !is_single_path_component(&candidate.name) {
            return Err(io::Error::other(
                "refusing to remove an unsafe debug release candidate",
            ));
        }
        let expected = paths.debug_release_root.join(&candidate.name);
        if candidate.path != expected {
            return Err(io::Error::other(
                "debug release candidate escaped its release root",
            ));
        }
        let metadata = fs::symlink_metadata(&expected)?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(io::Error::other(format!(
                "debug release candidate is not a direct directory: {}",
                expected.display()
            )));
        }
        fs::remove_dir_all(&expected)?;
    }
    Ok(())
}

fn directory_bytes(path: &Path) -> io::Result<u64> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(0),
        Err(error) => return Err(error),
    };
    if metadata.file_type().is_symlink() {
        return Ok(0);
    }
    if metadata.is_file() {
        return Ok(metadata.len());
    }
    if !metadata.is_dir() {
        return Ok(0);
    }
    let mut bytes = 0u64;
    for entry in fs::read_dir(path)? {
        bytes = bytes.saturating_add(directory_bytes(&entry?.path())?);
    }
    Ok(bytes)
}

fn is_single_path_component(name: &str) -> bool {
    !name.is_empty() && Path::new(name).components().count() == 1 && name != "." && name != ".."
}

fn format_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut value = bytes as f64;
    let mut index = 0;
    while value >= 1024.0 && index + 1 < UNITS.len() {
        value /= 1024.0;
        index += 1;
    }
    if index == 0 {
        format!("{bytes} {}", UNITS[index])
    } else {
        format!("{value:.1} {}", UNITS[index])
    }
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

fn print_prune_usage() {
    println!(
        "usage: cargo xtask storage-prune-debug [--keep N] [--apply]\n\
         Reports obsolete immutable debug releases by default. `--apply` permanently removes them.\n\
         The current release is always retained; close any old debug app before applying cleanup."
    );
}

#[cfg(test)]
mod tests;

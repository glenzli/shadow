use std::{
    env,
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
};

const DEFAULT_FIXTURE_DIRECTORY: &str = "local-reference/sample-assets/dng";
const DEFAULT_RAW_FIXTURE_DIRECTORY: &str = "local-reference/sample-assets/raw";

pub(super) fn resolve_fixture_directory(
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

pub(super) fn resolve_raw_fixture_directory(repository_root: &Path) -> io::Result<PathBuf> {
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

pub(super) fn first_supported_raw(directory: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    collect_supported_raws(directory, &mut candidates);
    candidates.sort();
    candidates.into_iter().next()
}

pub(super) fn collect_supported_raws(directory: &Path, paths: &mut Vec<PathBuf>) {
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

pub(super) fn materialize_raw_probe(source: &Path, destination: &Path) -> io::Result<()> {
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

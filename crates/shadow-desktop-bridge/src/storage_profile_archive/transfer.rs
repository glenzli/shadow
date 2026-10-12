use std::{
    fs::{self, File, FileTimes, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::atomic::Ordering,
    time::Duration,
};

use anyhow::{Context, Result, bail};
use rusqlite::{
    Connection, OpenFlags,
    backup::{Backup, StepResult},
};
use sha2::{Digest, Sha256};
use shadow_domain::AssetLocation;
use shadow_native_path::{current_platform, native_location, native_path_from_location};
use tempfile::TempDir;

use super::{
    ProfileArchiveControl,
    manifest::{self, ArchiveManifest},
};

pub(super) fn regular_file(path: &Path) -> Result<()> {
    if !fs::symlink_metadata(path)?.file_type().is_file() {
        bail!("library entry is not a regular file: {}", path.display());
    }
    Ok(())
}

pub(super) fn contained_file(root: &Path, path: &Path) -> Result<()> {
    regular_file(path)?;
    if !path.canonicalize()?.starts_with(root) {
        bail!("library entry leaves its directory: {}", path.display());
    }
    Ok(())
}

pub(super) fn inventory(root: &Path) -> Result<Vec<PathBuf>> {
    inventory_with_policy(root, true)
}

pub(super) fn payload_inventory(root: &Path) -> Result<Vec<PathBuf>> {
    inventory_with_policy(root, false)
}

fn inventory_with_policy(root: &Path, skip_ephemeral: bool) -> Result<Vec<PathBuf>> {
    fn walk(
        root: &Path,
        relative: &Path,
        depth: usize,
        skip_ephemeral: bool,
        out: &mut Vec<PathBuf>,
    ) -> Result<()> {
        if depth > 128 || out.len() > 1_000_000 {
            bail!("library exceeds archive traversal limits");
        }
        for item in fs::read_dir(root.join(relative))? {
            let item = item?;
            let path = relative.join(item.file_name());
            let kind = item.file_type()?;
            if kind.is_symlink() {
                bail!(
                    "library backup cannot follow symbolic links: {}",
                    path.display()
                );
            }
            if kind.is_dir() {
                walk(root, &path, depth + 1, skip_ephemeral, out)?;
            } else if kind.is_file() {
                let name = item.file_name().to_string_lossy().into_owned();
                if skip_ephemeral
                    && (name.ends_with(".lock")
                        || name.ends_with("-wal")
                        || name.ends_with("-shm")
                        || name.ends_with("-journal"))
                {
                    continue;
                }
                out.push(path);
            } else {
                bail!("library contains an unsupported filesystem entry");
            }
        }
        Ok(())
    }
    let mut files = Vec::new();
    walk(root, Path::new(""), 0, skip_ephemeral, &mut files)?;
    files.sort();
    Ok(files)
}

pub(super) fn digest(path: &Path, control: &ProfileArchiveControl) -> Result<(u64, String)> {
    let mut input = File::open(path)?;
    let mut buffer = vec![0u8; 256 * 1024];
    let mut hash = Sha256::new();
    let mut count = 0u64;
    loop {
        control.check()?;
        let size = input.read(&mut buffer)?;
        if size == 0 {
            break;
        }
        hash.update(&buffer[..size]);
        count = count
            .checked_add(size as u64)
            .context("library file size overflow")?;
    }
    Ok((count, format!("{:x}", hash.finalize())))
}

pub(super) fn copy(
    source: &Path,
    target: &Path,
    control: &ProfileArchiveControl,
    modified: Option<&manifest::ArchiveTimestamp>,
) -> Result<()> {
    regular_file(source)?;
    fs::create_dir_all(target.parent().context("target parent missing")?)?;
    let mut input = File::open(source)?;
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(target)?;
    let mut buffer = vec![0u8; 256 * 1024];
    loop {
        control.check()?;
        let size = input.read(&mut buffer)?;
        if size == 0 {
            break;
        }
        output.write_all(&buffer[..size])?;
    }
    let modified = modified
        .map(manifest::ArchiveTimestamp::time)
        .transpose()?
        .unwrap_or(fs::metadata(source)?.modified()?);
    output.set_times(FileTimes::new().set_modified(modified))?;
    output.sync_all()?;
    Ok(())
}

fn database(path: &Path) -> Result<Option<Connection>> {
    let mut header = [0u8; 16];
    if File::open(path)?.read(&mut header)? != 16 || &header != b"SQLite format 3\0" {
        if matches!(
            path.extension().and_then(|part| part.to_str()),
            Some("sqlite" | "sqlite3" | "db")
        ) {
            bail!("library database header is invalid: {}", path.display());
        }
        return Ok(None);
    }
    let connection = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    connection.busy_timeout(Duration::from_secs(5))?;
    connection.execute_batch("PRAGMA query_only=ON;")?;
    Ok(Some(connection))
}

pub(super) fn verify_database(path: &Path, catalog: bool) -> Result<()> {
    if catalog {
        shadow_catalog::verify_catalog_backup(path)?;
        return Ok(());
    }
    let connection = database(path)?.context("backup database header is invalid")?;
    let result: String = connection.query_row("PRAGMA integrity_check", [], |row| row.get(0))?;
    if result != "ok"
        || connection
            .prepare("PRAGMA foreign_key_check")?
            .query([])?
            .next()?
            .is_some()
    {
        bail!(
            "backup database failed its integrity check: {}",
            path.display()
        );
    }
    Ok(())
}

fn snapshot_database(
    input: &Connection,
    target: &Path,
    control: &ProfileArchiveControl,
) -> Result<()> {
    fs::create_dir_all(target.parent().context("database parent missing")?)?;
    let mut output = Connection::open(target)?;
    let backup = Backup::new(input, &mut output)?;
    let mut busy_steps = 0;
    loop {
        control.check()?;
        match backup.step(256)? {
            StepResult::Done => break,
            StepResult::More => {}
            StepResult::Busy | StepResult::Locked => {
                busy_steps += 1;
                if busy_steps > 500 {
                    bail!("database remains busy during library backup");
                }
                std::thread::sleep(Duration::from_millis(10));
            }
            _ => bail!("unknown SQLite backup state"),
        }
    }
    drop(backup);
    output.execute_batch("PRAGMA journal_mode=DELETE;")?;
    drop(output);
    Ok(())
}

pub(super) fn new_directory(parent: &Path, source: &Path, prefix: &str) -> Result<TempDir> {
    if !parent.is_absolute() {
        bail!("choose an absolute destination directory");
    }
    let parent = parent
        .canonicalize()
        .context("open destination directory")?;
    if !parent.is_dir() || overlaps_inside(&parent, source) {
        bail!("destination must be outside the source library or backup");
    }
    tempfile::Builder::new()
        .prefix(prefix)
        .tempdir_in(parent)
        .context("reserve a new library directory")
}

pub(super) fn overlaps_inside(path: &Path, source: &Path) -> bool {
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    {
        let p = path.to_string_lossy().to_lowercase();
        let s = source.to_string_lossy().to_lowercase();
        p == s || p.starts_with(&(s + std::path::MAIN_SEPARATOR_STR))
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    path.starts_with(source)
}

pub(super) fn sync_directory(path: &Path) -> Result<()> {
    #[cfg(unix)]
    File::open(path)?.sync_all()?;
    Ok(())
}

pub(super) fn sync_tree(root: &Path, control: &ProfileArchiveControl) -> Result<()> {
    fn sync(root: &Path, depth: usize, control: &ProfileArchiveControl) -> Result<()> {
        control.check()?;
        if depth > 128 {
            bail!("library directory depth exceeds its limit");
        }
        for entry in fs::read_dir(root)? {
            let entry = entry?;
            if entry.file_type()?.is_dir() {
                sync(&entry.path(), depth + 1, control)?;
            }
        }
        sync_directory(root)
    }
    sync(root, 0, control)
}

pub(super) fn snapshot(
    source: &Path,
    parent: &Path,
    expected_id: &str,
    control: &ProfileArchiveControl,
) -> Result<(TempDir, ArchiveManifest)> {
    let source = source.canonicalize().context("open source library")?;
    let profile = manifest::profile(&source, expected_id)?;
    let files = inventory(&source)?;
    if !files.contains(&PathBuf::from("catalog.sqlite")) {
        bail!("source library has no desktop catalog");
    }
    let directory = new_directory(parent, &source, "Shadow-Backup-")?;
    let payload = directory.path().join("payload");
    fs::create_dir(&payload)?;
    let mut manifest = ArchiveManifest {
        schema_version: 1,
        source_root: native_location(&source),
        profile,
        byte_count: 0,
        files: Vec::new(),
        owned_locations: Vec::new(),
    };
    let mut databases = Vec::new();
    control.total.store(files.len() as u64, Ordering::Release);
    control.completed.store(0, Ordering::Release);
    for relative in &files {
        control.check()?;
        let input = source.join(relative);
        let output = payload.join(relative);
        let modified = manifest::ArchiveTimestamp::from_time(fs::metadata(&input)?.modified()?);
        contained_file(&source, &input)?;
        let database = database(&input)?;
        let sqlite = database.is_some();
        if let Some(connection) = database {
            let version: i64 = connection.query_row("PRAGMA data_version", [], |row| row.get(0))?;
            snapshot_database(&connection, &output, control)?;
            verify_database(&output, relative == Path::new("catalog.sqlite"))?;
            databases.push((relative.clone(), connection, version));
        } else {
            copy(&input, &output, control, Some(&modified))?;
        }
        let (count, hash) = digest(&output, control)?;
        manifest.byte_count = manifest
            .byte_count
            .checked_add(count)
            .context("library size overflow")?;
        manifest
            .files
            .push(manifest::file(relative, count, hash, sqlite, modified));
        control.completed.fetch_add(1, Ordering::Release);
    }
    // Detect a writer or changed attachment across the whole consistency cut.
    if inventory(&source)? != files {
        bail!("library changed during backup; retry after current work finishes");
    }
    for (relative, connection, version) in databases {
        let after: i64 = connection.query_row("PRAGMA data_version", [], |row| row.get(0))?;
        if after != version {
            bail!("database changed during backup: {}", relative.display());
        }
    }
    for entry in &manifest.files {
        if !entry.sqlite {
            let (count, hash) =
                digest(&source.join(manifest::relative_path(&entry.path)?), control)?;
            if count != entry.byte_count || hash != entry.sha256 {
                bail!("attachment changed during backup");
            }
            let path = source.join(manifest::relative_path(&entry.path)?);
            if manifest::ArchiveTimestamp::from_time(fs::metadata(path)?.modified()?)
                != entry.modified
            {
                bail!("attachment metadata changed during backup");
            }
        }
    }
    control.check()?;
    manifest.owned_locations = owned_locations(&source, &payload.join("catalog.sqlite"))?;
    sync_tree(&payload, control)?;
    let bytes = serde_json::to_vec_pretty(&manifest)?;
    if bytes.len() > 128 * 1024 * 1024 {
        bail!("backup manifest exceeds 128 MiB");
    }
    let marker = directory.path().join(manifest::MANIFEST);
    let mut marker_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(marker)?;
    marker_file.write_all(&bytes)?;
    marker_file.sync_all()?;
    manifest::verify(directory.path(), control)?;
    sync_directory(directory.path())?;
    sync_directory(parent)?;
    Ok((directory, manifest))
}

fn owned_locations(source: &Path, catalog: &Path) -> Result<Vec<manifest::OwnedLocation>> {
    let connection = Connection::open_with_flags(catalog, OpenFlags::SQLITE_OPEN_READ_ONLY)?;
    let mut owned = Vec::new();
    for table in ["locations", "library_sources"] {
        let mut query = connection.prepare(&format!(
            "SELECT id,native_path FROM {table} WHERE platform=?1"
        ))?;
        let rows = query.query_map([current_platform().as_str()], |row| {
            Ok((row.get::<_, Vec<u8>>(0)?, row.get::<_, Vec<u8>>(1)?))
        })?;
        for row in rows {
            let (id, bytes) = row?;
            let path =
                native_path_from_location(&AssetLocation::new(current_platform(), bytes, ""))?;
            // Resolve aliases while the source is available. Restore must also
            // work after that source is gone, including /var vs /private/var.
            let resolved = path.canonicalize().unwrap_or(path);
            if let Ok(relative) = resolved.strip_prefix(source) {
                if relative.as_os_str().is_empty() {
                    bail!("a source location cannot be the whole profile storage directory");
                }
                owned.push(manifest::OwnedLocation {
                    table: table.to_owned(),
                    id,
                    relative: native_location(relative),
                });
            }
        }
    }
    Ok(owned)
}

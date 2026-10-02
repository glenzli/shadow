use std::{
    fs::{self, File},
    path::{Path, PathBuf},
    time::Duration,
};

use rusqlite::{Connection, OpenFlags, backup::Backup};
use tempfile::TempPath;
use thiserror::Error;
use uuid::Uuid;

use crate::{
    CatalogStats,
    row_codec::count_rows,
    schema::{SCHEMA_VERSION, catalog_tables_exist, current_version},
};

mod restore;

pub use restore::{CatalogRestoreError, CatalogRestoreReceipt, restore_catalog_backup_offline};

const BACKUP_PAGES_PER_STEP: i32 = 256;
const BACKUP_STEP_PAUSE: Duration = Duration::from_millis(2);
const MAX_INTEGRITY_DIAGNOSTICS: u32 = 32;

/// Facts recovered by reopening a completed catalog backup read-only.
///
/// A successful value is a restore drill, not merely a report that `SQLite`
/// finished copying pages. It proves the backup is structurally readable,
/// belongs to the one current development schema, passes full database and
/// foreign-key checks, and retains the expected durable entity counts.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CatalogBackupVerification {
    pub schema_version: i64,
    pub stats: CatalogStats,
    pub page_count: u64,
    pub page_size: u64,
    pub database_bytes: u64,
}

/// Receipt returned after a verified partial backup has been durably published.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CatalogBackupReceipt {
    pub destination: PathBuf,
    pub verification: CatalogBackupVerification,
}

#[derive(Debug, Error)]
pub enum CatalogBackupError {
    #[error("catalog backup source and destination resolve to the same file: {0}")]
    SourceEqualsDestination(PathBuf),
    #[error("catalog backup destination already exists and will not be overwritten: {0}")]
    DestinationExists(PathBuf),
    #[error("catalog backup path has no file name: {0}")]
    MissingFileName(PathBuf),
    #[error("catalog backup {operation} failed for {path}: {source}")]
    Io {
        operation: &'static str,
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("catalog backup SQLite operation failed: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error(
        "catalog backup uses unsupported schema revision {actual}; expected current revision {expected}"
    )]
    SchemaVersionMismatch { expected: i64, actual: i64 },
    #[error("catalog backup is missing one or more required catalog tables")]
    MissingCatalogTables,
    #[error("catalog backup failed SQLite integrity_check: {0:?}")]
    IntegrityCheckFailed(Vec<String>),
    #[error("catalog backup contains {0} foreign-key violation(s)")]
    ForeignKeyCheckFailed(u64),
    #[error("catalog backup reported an invalid negative {field}: {value}")]
    InvalidNumericField { field: &'static str, value: i64 },
}

/// Creates a consistent backup of a live file-backed catalog.
///
/// The source is opened through a separate read-only `SQLite` connection, so
/// callers can execute this function on a background worker without borrowing
/// or blocking the catalog's single writer actor for the duration of the copy.
/// `SQLite`'s Online Backup API includes committed WAL state. Shadow writes to a
/// unique partial file in the destination directory, reopens that file for a
/// complete restore drill, synchronizes it, and only then publishes the final
/// path. Publication refuses to replace existing destinations, including symlinks.
///
/// # Errors
///
/// Returns [`CatalogBackupError`] when paths cannot be prepared, `SQLite` cannot
/// produce a consistent copy, verification fails, or durable publication
/// cannot complete. Failed partial files are removed on a best-effort basis.
pub fn create_catalog_backup(
    source: &Path,
    destination: &Path,
) -> Result<CatalogBackupReceipt, CatalogBackupError> {
    let source = canonicalize_source(source)?;
    let destination = absolute_destination(destination)?;
    if destination == source {
        return Err(CatalogBackupError::SourceEqualsDestination(source));
    }
    if destination.exists() {
        return Err(CatalogBackupError::DestinationExists(destination));
    }

    let partial = partial_backup_path(&destination)?;
    let result = create_and_verify_partial(&source, &partial);
    let verification = match result {
        Ok(verification) => verification,
        Err(error) => {
            let _ = fs::remove_file(&partial);
            return Err(error);
        }
    };

    publish_backup(&partial, &destination)?;
    sync_parent_directory(&destination)?;

    Ok(CatalogBackupReceipt {
        destination,
        verification,
    })
}

fn publish_backup(partial: &Path, destination: &Path) -> Result<(), CatalogBackupError> {
    // The final operation must reserve the destination without replacement. An
    // exists-then-rename check can lose a race or replace a dangling symlink.
    let temporary = TempPath::try_from_path(partial).map_err(|source| CatalogBackupError::Io {
        operation: "prepare publication",
        path: partial.to_path_buf(),
        source,
    })?;
    temporary.persist_noclobber(destination).map_err(|error| {
        if error.error.kind() == std::io::ErrorKind::AlreadyExists {
            CatalogBackupError::DestinationExists(destination.to_path_buf())
        } else {
            CatalogBackupError::Io {
                operation: "publish",
                path: destination.to_path_buf(),
                source: error.error,
            }
        }
    })
}

/// Reopens an existing backup read-only and performs the same restore drill
/// used before publication.
///
/// This function is intentionally independent of [`crate::Catalog::open`]:
/// verification must never initialize, migrate, reset, or otherwise mutate
/// the file it is auditing.
///
/// # Errors
///
/// Returns [`CatalogBackupError`] for unreadable/corrupt `SQLite` data, an
/// unsupported development schema, failed integrity or foreign-key checks, or
/// invalid database accounting.
pub fn verify_catalog_backup(path: &Path) -> Result<CatalogBackupVerification, CatalogBackupError> {
    let connection = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    connection.busy_timeout(Duration::from_secs(5))?;
    connection.execute_batch("PRAGMA query_only = ON; PRAGMA foreign_keys = ON;")?;

    let integrity_messages = integrity_diagnostics(&connection)?;
    if integrity_messages.as_slice() != ["ok"] {
        return Err(CatalogBackupError::IntegrityCheckFailed(integrity_messages));
    }
    let foreign_key_violations = foreign_key_violation_count(&connection)?;
    if foreign_key_violations != 0 {
        return Err(CatalogBackupError::ForeignKeyCheckFailed(
            foreign_key_violations,
        ));
    }
    if !catalog_tables_exist(&connection)? {
        return Err(CatalogBackupError::MissingCatalogTables);
    }
    let schema_version = current_version(&connection)?;
    if schema_version != SCHEMA_VERSION {
        return Err(CatalogBackupError::SchemaVersionMismatch {
            expected: SCHEMA_VERSION,
            actual: schema_version,
        });
    }

    let page_count = non_negative_u64(
        connection.query_row("PRAGMA page_count", [], |row| row.get(0))?,
        "page_count",
    )?;
    let page_size = non_negative_u64(
        connection.query_row("PRAGMA page_size", [], |row| row.get(0))?,
        "page_size",
    )?;
    let database_bytes = fs::metadata(path)
        .map_err(|source| CatalogBackupError::Io {
            operation: "read metadata",
            path: path.to_path_buf(),
            source,
        })?
        .len();
    let locations_needing_revalidation: i64 = connection.query_row(
        "SELECT COUNT(*) FROM locations WHERE status = 'needs_revalidation'",
        [],
        |row| row.get(0),
    )?;

    Ok(CatalogBackupVerification {
        schema_version,
        stats: CatalogStats {
            photos: count_rows(&connection, "photos")?,
            representations: count_rows(&connection, "representations")?,
            locations: count_rows(&connection, "locations")?,
            locations_needing_revalidation: non_negative_u64(
                locations_needing_revalidation,
                "locations_needing_revalidation",
            )?,
        },
        page_count,
        page_size,
        database_bytes,
    })
}

pub(super) fn create_and_verify_partial(
    source: &Path,
    partial: &Path,
) -> Result<CatalogBackupVerification, CatalogBackupError> {
    let source_connection = Connection::open_with_flags(
        source,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    source_connection.busy_timeout(Duration::from_secs(5))?;
    let mut destination_connection = Connection::open(partial)?;
    {
        let backup = Backup::new(&source_connection, &mut destination_connection)?;
        backup.run_to_completion(BACKUP_PAGES_PER_STEP, BACKUP_STEP_PAUSE, None)?;
    }
    drop(destination_connection);
    drop(source_connection);

    let verification = verify_catalog_backup(partial)?;
    File::open(partial)
        .and_then(|file| file.sync_all())
        .map_err(|source| CatalogBackupError::Io {
            operation: "synchronize partial backup",
            path: partial.to_path_buf(),
            source,
        })?;
    Ok(verification)
}

fn canonicalize_source(source: &Path) -> Result<PathBuf, CatalogBackupError> {
    source
        .canonicalize()
        .map_err(|error| CatalogBackupError::Io {
            operation: "resolve source",
            path: source.to_path_buf(),
            source: error,
        })
}

pub(super) fn absolute_destination(destination: &Path) -> Result<PathBuf, CatalogBackupError> {
    let file_name = destination
        .file_name()
        .ok_or_else(|| CatalogBackupError::MissingFileName(destination.to_path_buf()))?;
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent).map_err(|source| CatalogBackupError::Io {
        operation: "create destination directory",
        path: parent.to_path_buf(),
        source,
    })?;
    let canonical_parent = parent
        .canonicalize()
        .map_err(|source| CatalogBackupError::Io {
            operation: "resolve destination directory",
            path: parent.to_path_buf(),
            source,
        })?;
    Ok(canonical_parent.join(file_name))
}

fn partial_backup_path(destination: &Path) -> Result<PathBuf, CatalogBackupError> {
    let file_name = destination
        .file_name()
        .ok_or_else(|| CatalogBackupError::MissingFileName(destination.to_path_buf()))?
        .to_string_lossy();
    Ok(destination.with_file_name(format!(".{file_name}.shadow-partial-{}", Uuid::now_v7())))
}

fn integrity_diagnostics(connection: &Connection) -> Result<Vec<String>, CatalogBackupError> {
    let mut statement = connection.prepare(&format!(
        "PRAGMA integrity_check({MAX_INTEGRITY_DIAGNOSTICS})"
    ))?;
    let rows = statement.query_map([], |row| row.get::<_, String>(0))?;
    rows.collect::<Result<Vec<_>, _>>().map_err(Into::into)
}

fn foreign_key_violation_count(connection: &Connection) -> Result<u64, CatalogBackupError> {
    let mut statement = connection.prepare("PRAGMA foreign_key_check")?;
    let mut rows = statement.query([])?;
    let mut count = 0_u64;
    while rows.next()?.is_some() {
        count = count.saturating_add(1);
    }
    Ok(count)
}

fn non_negative_u64(value: i64, field: &'static str) -> Result<u64, CatalogBackupError> {
    u64::try_from(value).map_err(|_| CatalogBackupError::InvalidNumericField { field, value })
}

#[cfg(unix)]
pub(super) fn sync_parent_directory(path: &Path) -> Result<(), CatalogBackupError> {
    let parent = path
        .parent()
        .ok_or_else(|| CatalogBackupError::MissingFileName(path.to_path_buf()))?;
    File::open(parent)
        .and_then(|directory| directory.sync_all())
        .map_err(|source| CatalogBackupError::Io {
            operation: "synchronize destination directory",
            path: parent.to_path_buf(),
            source,
        })
}

#[cfg(not(unix))]
pub(super) fn sync_parent_directory(_path: &Path) -> Result<(), CatalogBackupError> {
    Ok(())
}

#[cfg(test)]
mod tests;

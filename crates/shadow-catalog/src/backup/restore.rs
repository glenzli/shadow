//! Offline Catalog restore publication and rollback preservation.

use std::{
    ffi::OsString,
    fs::{self, File},
    path::{Path, PathBuf},
};

use thiserror::Error;
use uuid::Uuid;

use super::{
    CatalogBackupError, CatalogBackupVerification, absolute_destination, create_and_verify_partial,
    sync_parent_directory, verify_catalog_backup,
};

/// Receipt for one verified offline restore publication.
///
/// When an older Catalog or SQLite sidecar existed, `rollback_directory`
/// retains it byte-for-byte. Shadow never silently deletes that recovery set.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CatalogRestoreReceipt {
    pub destination: PathBuf,
    pub source_verification: CatalogBackupVerification,
    pub restored_verification: CatalogBackupVerification,
    pub rollback_directory: Option<PathBuf>,
}

#[derive(Debug, Error)]
pub enum CatalogRestoreError {
    #[error(transparent)]
    Backup(#[from] CatalogBackupError),
    #[error("Catalog restore source and destination resolve to the same file: {0}")]
    SourceEqualsDestination(PathBuf),
    #[error("Catalog restore {operation} failed for {path}: {source}")]
    Io {
        operation: &'static str,
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error(
        "restored Catalog verification failed and the previous files were restored from {rollback_directory:?}: {source}"
    )]
    VerificationRolledBack {
        rollback_directory: Option<PathBuf>,
        #[source]
        source: CatalogBackupError,
    },
    #[error("Catalog restore rollback failed for {path}: {source}; original failure: {original}")]
    RollbackFailed {
        original: String,
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
}

/// Restores a verified backup over an offline Catalog path.
///
/// This is intentionally an offline boundary. The desktop must invoke it
/// before opening its Catalog, and operator callers must stop every Shadow
/// process that could use the destination. The restore deliberately does not
/// open or checkpoint the old database before displacement, because doing so
/// could mutate the exact database, WAL, or shared-memory evidence that must
/// remain available for rollback.
///
/// The backup is first cloned into a unique partial file and verified again.
/// Existing database, WAL, and shared-memory files are moved into a unique
/// rollback directory before publication. The final destination is reopened
/// read-only for the complete restore drill. Any publication or verification
/// failure restores the displaced files.
///
/// # Errors
///
/// Returns [`CatalogRestoreError`] when the backup is invalid, publication
/// fails, or rollback cannot restore the original file set. The offline
/// precondition is owned by the caller; this function does not open or probe
/// the old Catalog in a way that could mutate its rollback evidence.
pub fn restore_catalog_backup_offline(
    backup: &Path,
    destination: &Path,
) -> Result<CatalogRestoreReceipt, CatalogRestoreError> {
    let backup = backup
        .canonicalize()
        .map_err(|source| CatalogRestoreError::Io {
            operation: "resolve backup",
            path: backup.to_path_buf(),
            source,
        })?;
    let destination = absolute_destination(destination)?;
    if same_file_path(&backup, &destination) {
        return Err(CatalogRestoreError::SourceEqualsDestination(backup));
    }

    let partial = restore_partial_path(&destination)?;
    let source_verification = match create_and_verify_partial(&backup, &partial) {
        Ok(verification) => verification,
        Err(error) => {
            let _ = fs::remove_file(&partial);
            return Err(error.into());
        }
    };

    let displaced = match DisplacedCatalog::move_from(&destination) {
        Ok(displaced) => displaced,
        Err(error) => {
            let _ = fs::remove_file(&partial);
            return Err(error);
        }
    };
    if let Err(source) = fs::rename(&partial, &destination) {
        let original = format!("publish restored Catalog: {source}");
        if let Err((path, source)) = displaced.rollback(&destination) {
            return Err(CatalogRestoreError::RollbackFailed {
                original,
                path,
                source,
            });
        }
        return Err(CatalogRestoreError::Io {
            operation: "publish restored Catalog",
            path: destination,
            source,
        });
    }

    if let Err(source) = File::open(&destination).and_then(|file| file.sync_all()) {
        let original = format!("synchronize restored Catalog: {source}");
        if let Err((path, source)) = displaced.rollback(&destination) {
            return Err(CatalogRestoreError::RollbackFailed {
                original,
                path,
                source,
            });
        }
        return Err(CatalogRestoreError::Io {
            operation: "synchronize restored Catalog",
            path: destination,
            source,
        });
    }

    if let Err(error) = sync_parent_directory(&destination) {
        let original = error.to_string();
        if let Err((path, source)) = displaced.rollback(&destination) {
            return Err(CatalogRestoreError::RollbackFailed {
                original,
                path,
                source,
            });
        }
        return Err(error.into());
    }

    let restored_verification = match verify_catalog_backup(&destination) {
        Ok(verification) => verification,
        Err(source) => {
            let rollback_directory = displaced.rollback_directory().map(Path::to_path_buf);
            let original = source.to_string();
            if let Err((path, source)) = displaced.rollback(&destination) {
                return Err(CatalogRestoreError::RollbackFailed {
                    original,
                    path,
                    source,
                });
            }
            return Err(CatalogRestoreError::VerificationRolledBack {
                rollback_directory,
                source,
            });
        }
    };
    Ok(CatalogRestoreReceipt {
        destination,
        source_verification,
        restored_verification,
        rollback_directory: displaced.rollback_directory().map(Path::to_path_buf),
    })
}

fn same_file_path(left: &Path, right: &Path) -> bool {
    if left == right {
        return true;
    }
    right.canonicalize().is_ok_and(|path| path == left)
}

fn restore_partial_path(destination: &Path) -> Result<PathBuf, CatalogRestoreError> {
    let file_name = destination
        .file_name()
        .ok_or_else(|| CatalogBackupError::MissingFileName(destination.to_path_buf()))?
        .to_string_lossy();
    Ok(destination.with_file_name(format!(
        ".{file_name}.shadow-restore-partial-{}",
        Uuid::now_v7()
    )))
}

fn sidecar_path(destination: &Path, suffix: &str) -> PathBuf {
    let mut path = OsString::from(destination.as_os_str());
    path.push(suffix);
    PathBuf::from(path)
}

struct DisplacedCatalog {
    rollback_directory: Option<PathBuf>,
    moved: Vec<(PathBuf, PathBuf)>,
}

impl DisplacedCatalog {
    fn move_from(destination: &Path) -> Result<Self, CatalogRestoreError> {
        let components = [
            destination.to_path_buf(),
            sidecar_path(destination, "-wal"),
            sidecar_path(destination, "-shm"),
        ];
        if !components.iter().any(|path| path.exists()) {
            return Ok(Self {
                rollback_directory: None,
                moved: Vec::new(),
            });
        }
        let file_name = destination
            .file_name()
            .ok_or_else(|| CatalogBackupError::MissingFileName(destination.to_path_buf()))?
            .to_string_lossy();
        let rollback_directory = destination.with_file_name(format!(
            ".{file_name}.shadow-pre-restore-{}",
            Uuid::now_v7()
        ));
        fs::create_dir(&rollback_directory).map_err(|source| CatalogRestoreError::Io {
            operation: "create rollback directory",
            path: rollback_directory.clone(),
            source,
        })?;
        let mut displaced = Self {
            rollback_directory: Some(rollback_directory),
            moved: Vec::new(),
        };
        for original in components.iter().filter(|path| path.exists()) {
            if let Err(source) = File::open(original).and_then(|file| file.sync_all()) {
                let original_error = format!("synchronize existing Catalog component: {source}");
                if let Err((path, source)) = displaced.restore_moved() {
                    return Err(CatalogRestoreError::RollbackFailed {
                        original: original_error,
                        path,
                        source,
                    });
                }
                return Err(CatalogRestoreError::Io {
                    operation: "synchronize existing Catalog component",
                    path: original.clone(),
                    source,
                });
            }
            let moved = displaced
                .rollback_directory
                .as_ref()
                .expect("rollback directory exists for displaced files")
                .join(
                    original
                        .file_name()
                        .expect("Catalog component always has a file name"),
                );
            if let Err(source) = fs::rename(original, &moved) {
                let original_error = format!("move existing Catalog component: {source}");
                if let Err((path, source)) = displaced.restore_moved() {
                    return Err(CatalogRestoreError::RollbackFailed {
                        original: original_error,
                        path,
                        source,
                    });
                }
                return Err(CatalogRestoreError::Io {
                    operation: "move existing Catalog component",
                    path: original.clone(),
                    source,
                });
            }
            displaced.moved.push((original.clone(), moved));
        }
        if let Some(directory) = displaced.rollback_directory() {
            if let Err(source) = sync_directory(directory) {
                let original_error = format!("synchronize rollback directory: {source}");
                if let Err((path, source)) = displaced.restore_moved() {
                    return Err(CatalogRestoreError::RollbackFailed {
                        original: original_error,
                        path,
                        source,
                    });
                }
                return Err(CatalogRestoreError::Io {
                    operation: "synchronize rollback directory",
                    path: directory.to_path_buf(),
                    source,
                });
            }
        }
        if let Err(error) = sync_parent_directory(destination) {
            let (path, source) = io_from_backup_error(destination, error);
            let original_error = format!("synchronize displaced Catalog publication: {source}");
            if let Err((path, source)) = displaced.restore_moved() {
                return Err(CatalogRestoreError::RollbackFailed {
                    original: original_error,
                    path,
                    source,
                });
            }
            return Err(CatalogRestoreError::Io {
                operation: "synchronize displaced Catalog publication",
                path,
                source,
            });
        }
        Ok(displaced)
    }

    fn rollback_directory(&self) -> Option<&Path> {
        self.rollback_directory.as_deref()
    }

    fn rollback(&self, destination: &Path) -> Result<(), (PathBuf, std::io::Error)> {
        if destination.exists() {
            if let Some(directory) = self.rollback_directory() {
                let failed = directory.join(format!(".failed-restored-catalog-{}", Uuid::now_v7()));
                fs::rename(destination, &failed)
                    .map_err(|source| (destination.to_path_buf(), source))?;
            } else {
                fs::remove_file(destination)
                    .map_err(|source| (destination.to_path_buf(), source))?;
            }
        }
        self.restore_moved()?;
        sync_parent_directory(destination).map_err(|error| io_from_backup_error(destination, error))
    }

    fn restore_moved(&self) -> Result<(), (PathBuf, std::io::Error)> {
        for (original, moved) in self.moved.iter().rev() {
            fs::rename(moved, original).map_err(|source| (original.clone(), source))?;
        }
        Ok(())
    }
}

fn io_from_backup_error(
    destination: &Path,
    error: CatalogBackupError,
) -> (PathBuf, std::io::Error) {
    match error {
        CatalogBackupError::Io { path, source, .. } => (path, source),
        other => (
            destination.to_path_buf(),
            std::io::Error::other(other.to_string()),
        ),
    }
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), std::io::Error> {
    File::open(path)?.sync_all()
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), std::io::Error> {
    Ok(())
}

#[cfg(test)]
mod tests;

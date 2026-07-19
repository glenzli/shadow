use std::{
    fs,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{
    CatalogError, CatalogStore, ImportSessionState, RegisterAsset, RegistrationStatus,
};
use shadow_domain::{ImportSessionId, RepresentationKind};
use thiserror::Error;

use crate::native_path::{NativePathError, decode_location, encode_location};

#[derive(Debug, Error)]
pub enum ScanError {
    #[error("cannot resolve the current directory: {0}")]
    CurrentDirectory(#[source] std::io::Error),
    #[error("catalog operation failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("cannot decode import root path: {0}")]
    NativePath(#[from] NativePathError),
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ScanIssue {
    pub path: PathBuf,
    pub message: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ScanReport {
    pub session_id: ImportSessionId,
    pub files_seen: u64,
    pub supported_files: u64,
    pub inserted: u64,
    pub unchanged: u64,
    pub needs_revalidation: u64,
    pub skipped: u64,
    pub issues: Vec<ScanIssue>,
}

/// Recursively scans supported photo files without following symlinks.
///
/// Individual filesystem read failures are accumulated in [`ScanReport`].
///
/// # Errors
///
/// Returns [`ScanError`] if the current directory cannot be resolved for a
/// relative root, or if a catalog transaction fails.
pub fn scan_folder<C: CatalogStore + ?Sized>(
    catalog: &mut C,
    root: &Path,
) -> Result<ScanReport, ScanError> {
    let root = if root.is_absolute() {
        root.to_path_buf()
    } else {
        std::env::current_dir()
            .map_err(ScanError::CurrentDirectory)?
            .join(root)
    };

    let now_ms = now_ms();
    let session_id = catalog.begin_import_session(&encode_location(&root), now_ms)?;
    run_scan_session(catalog, session_id, &root)
}

/// Resumes an interrupted import session by idempotently rescanning its root.
///
/// # Errors
///
/// Returns [`ScanError`] if the session is absent or terminal, its native path
/// belongs to another platform, or scanning/catalog operations fail.
pub fn resume_scan<C: CatalogStore + ?Sized>(
    catalog: &mut C,
    session_id: ImportSessionId,
) -> Result<ScanReport, ScanError> {
    let session = catalog.resume_import_session(session_id, now_ms())?;
    let root = decode_location(&session.root)?;
    run_scan_session(catalog, session_id, &root)
}

fn run_scan_session(
    catalog: &mut (impl CatalogStore + ?Sized),
    session_id: ImportSessionId,
    root: &Path,
) -> Result<ScanReport, ScanError> {
    let mut report = ScanReport {
        session_id,
        files_seen: 0,
        supported_files: 0,
        inserted: 0,
        unchanged: 0,
        needs_revalidation: 0,
        skipped: 0,
        issues: Vec::new(),
    };
    if let Err(error) = scan_directory(catalog, session_id, root, &mut report) {
        let message = error.to_string();
        let _ = catalog.finish_import_session(
            session_id,
            ImportSessionState::Failed,
            Some(&message),
            now_ms(),
        );
        return Err(error);
    }
    catalog.finish_import_session(session_id, ImportSessionState::Completed, None, now_ms())?;
    Ok(report)
}

fn scan_directory(
    catalog: &mut (impl CatalogStore + ?Sized),
    session_id: ImportSessionId,
    directory: &Path,
    report: &mut ScanReport,
) -> Result<(), ScanError> {
    let entries = match fs::read_dir(directory) {
        Ok(entries) => entries,
        Err(error) => {
            record_issue(catalog, session_id, directory, error.to_string(), report)?;
            return Ok(());
        }
    };

    for entry in entries {
        let entry = match entry {
            Ok(entry) => entry,
            Err(error) => {
                record_issue(catalog, session_id, directory, error.to_string(), report)?;
                continue;
            }
        };
        let path = entry.path();
        let file_type = match entry.file_type() {
            Ok(file_type) => file_type,
            Err(error) => {
                record_issue(catalog, session_id, &path, error.to_string(), report)?;
                continue;
            }
        };

        if file_type.is_symlink() {
            report.skipped += 1;
            continue;
        }
        if file_type.is_dir() {
            scan_directory(catalog, session_id, &path, report)?;
            continue;
        }
        if !file_type.is_file() {
            report.skipped += 1;
            continue;
        }

        report.files_seen += 1;
        let Some(kind) = representation_kind(&path) else {
            report.skipped += 1;
            continue;
        };
        report.supported_files += 1;

        let metadata = match entry.metadata() {
            Ok(metadata) => metadata,
            Err(error) => {
                record_issue(catalog, session_id, &path, error.to_string(), report)?;
                continue;
            }
        };
        let request = RegisterAsset {
            kind,
            location: encode_location(&path),
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            now_ms: system_time_ms(SystemTime::now()).unwrap_or_default(),
        };
        catalog.record_import_discovered(session_id, &request)?;
        let registered = catalog.register_import_asset(session_id, &request)?;
        match registered.status {
            RegistrationStatus::Inserted => report.inserted += 1,
            RegistrationStatus::Unchanged => report.unchanged += 1,
            RegistrationStatus::NeedsRevalidation => report.needs_revalidation += 1,
        }
    }

    Ok(())
}

fn record_issue(
    catalog: &mut (impl CatalogStore + ?Sized),
    session_id: ImportSessionId,
    path: &Path,
    message: String,
    report: &mut ScanReport,
) -> Result<(), ScanError> {
    catalog.record_import_issue(session_id, &encode_location(path), &message, now_ms())?;
    report.issues.push(ScanIssue {
        path: path.to_path_buf(),
        message,
    });
    Ok(())
}

fn representation_kind(path: &Path) -> Option<RepresentationKind> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    match extension.as_str() {
        "nef" | "nrw" | "cr2" | "cr3" | "arw" | "raf" | "orf" | "rw2" | "pef" | "srw" => {
            Some(RepresentationKind::OriginalRaw)
        }
        "dng" => Some(RepresentationKind::OriginalRaw),
        "jpg" | "jpeg" | "tif" | "tiff" | "png" | "heic" | "heif" => {
            Some(RepresentationKind::OriginalRaster)
        }
        _ => None,
    }
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}

fn now_ms() -> i64 {
    system_time_ms(SystemTime::now()).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use shadow_catalog::{Catalog, CatalogActor};
    use shadow_domain::{EntityId, PhotoId};

    #[test]
    fn scan_is_recursive_filtered_and_idempotent() {
        let root = std::env::temp_dir().join(format!("shadow-scan-{}", PhotoId::new_v7()));
        let nested = root.join("nested");
        fs::create_dir_all(&nested).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
        fs::write(nested.join("two.jpg"), b"jpeg").expect("write jpeg fixture");
        fs::write(root.join("notes.txt"), b"ignore").expect("write ignored fixture");

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let first = scan_folder(&mut catalog, &root).expect("first scan");
        let second = scan_folder(&mut catalog, &root).expect("second scan");

        assert_eq!(first.inserted, 2);
        assert_eq!(first.skipped, 1);
        assert_eq!(second.unchanged, 2);
        assert_eq!(catalog.stats().expect("stats").photos, 2);
        assert_eq!(
            catalog
                .import_session_summary(first.session_id)
                .expect("first session summary")
                .inserted,
            2
        );

        fs::remove_dir_all(&root).expect("remove fixture directory");
    }

    #[test]
    fn interrupted_session_can_be_resumed() {
        let root = std::env::temp_dir().join(format!("shadow-resume-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let session_id = catalog
            .begin_import_session(&encode_location(&root), now_ms())
            .expect("begin interrupted session");
        let report = resume_scan(&mut catalog, session_id).expect("resume scan");

        assert_eq!(report.session_id, session_id);
        assert_eq!(report.inserted, 1);
        assert_eq!(
            catalog
                .import_session_summary(session_id)
                .expect("session summary")
                .session
                .state,
            ImportSessionState::Completed
        );

        fs::remove_dir_all(&root).expect("remove fixture directory");
    }

    #[test]
    fn production_actor_keeps_scanning_off_the_writer_thread() {
        let test_id = PhotoId::new_v7();
        let root = std::env::temp_dir().join(format!("shadow-actor-scan-{test_id}"));
        let database_path = std::env::temp_dir().join(format!("shadow-actor-{test_id}.sqlite"));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");

        let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
        let mut handle = actor.handle();
        let report = scan_folder(&mut handle, &root).expect("scan through actor");
        assert_eq!(report.inserted, 1);
        assert_eq!(handle.stats().expect("stats").photos, 1);
        actor.shutdown().expect("shutdown actor");

        fs::remove_dir_all(&root).expect("remove fixture directory");
        fs::remove_file(&database_path).expect("remove test catalog");
        for extension in ["sqlite-wal", "sqlite-shm"] {
            let sidecar = database_path.with_extension(extension);
            if sidecar.exists() {
                fs::remove_file(sidecar).expect("remove catalog sidecar");
            }
        }
    }
}

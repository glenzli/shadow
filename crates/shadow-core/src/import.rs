use std::{
    fs,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{Catalog, CatalogError, RegisterAsset, RegistrationStatus};
use shadow_domain::RepresentationKind;
use thiserror::Error;

use crate::native_path::encode_location;

#[derive(Debug, Error)]
pub enum ScanError {
    #[error("cannot resolve the current directory: {0}")]
    CurrentDirectory(#[source] std::io::Error),
    #[error("catalog operation failed: {0}")]
    Catalog(#[from] CatalogError),
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ScanIssue {
    pub path: PathBuf,
    pub message: String,
}

#[derive(Debug, Default, Clone, Eq, PartialEq)]
pub struct ScanReport {
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
pub fn scan_folder(catalog: &mut Catalog, root: &Path) -> Result<ScanReport, ScanError> {
    let root = if root.is_absolute() {
        root.to_path_buf()
    } else {
        std::env::current_dir()
            .map_err(ScanError::CurrentDirectory)?
            .join(root)
    };

    let mut report = ScanReport::default();
    scan_directory(catalog, &root, &mut report)?;
    Ok(report)
}

fn scan_directory(
    catalog: &mut Catalog,
    directory: &Path,
    report: &mut ScanReport,
) -> Result<(), ScanError> {
    let entries = match fs::read_dir(directory) {
        Ok(entries) => entries,
        Err(error) => {
            report.issues.push(ScanIssue {
                path: directory.to_path_buf(),
                message: error.to_string(),
            });
            return Ok(());
        }
    };

    for entry in entries {
        let entry = match entry {
            Ok(entry) => entry,
            Err(error) => {
                report.issues.push(ScanIssue {
                    path: directory.to_path_buf(),
                    message: error.to_string(),
                });
                continue;
            }
        };
        let path = entry.path();
        let file_type = match entry.file_type() {
            Ok(file_type) => file_type,
            Err(error) => {
                report.issues.push(ScanIssue {
                    path,
                    message: error.to_string(),
                });
                continue;
            }
        };

        if file_type.is_symlink() {
            report.skipped += 1;
            continue;
        }
        if file_type.is_dir() {
            scan_directory(catalog, &path, report)?;
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
                report.issues.push(ScanIssue {
                    path,
                    message: error.to_string(),
                });
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
        match catalog.register_asset(&request)?.status {
            RegistrationStatus::Inserted => report.inserted += 1,
            RegistrationStatus::Unchanged => report.unchanged += 1,
            RegistrationStatus::NeedsRevalidation => report.needs_revalidation += 1,
        }
    }

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

#[cfg(test)]
mod tests {
    use super::*;
    use shadow_catalog::Catalog;
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

        fs::remove_dir_all(&root).expect("remove fixture directory");
    }
}

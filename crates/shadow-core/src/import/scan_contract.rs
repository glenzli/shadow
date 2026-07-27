//! Public folder-scan outcomes, progress snapshots, failures, and cancellation.

use std::{
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

use shadow_catalog::CatalogError;
use shadow_domain::ImportSessionId;
use thiserror::Error;

use crate::{DecodeInspectionError, native_path::NativePathError, performance::ScanPerformance};

#[derive(Debug, Error)]
pub enum ScanError {
    #[error("cannot resolve the current directory: {0}")]
    CurrentDirectory(#[source] std::io::Error),
    #[error("catalog operation failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("cannot decode import root path: {0}")]
    NativePath(#[from] NativePathError),
    #[error("cannot schedule source inspection: {0}")]
    DecodeInspection(#[from] DecodeInspectionError),
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ScanIssue {
    pub path: PathBuf,
    pub message: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ScanReport {
    pub session_id: ImportSessionId,
    pub completion: ScanCompletion,
    pub files_seen: u64,
    pub supported_files: u64,
    pub inserted: u64,
    pub unchanged: u64,
    pub needs_revalidation: u64,
    pub decode_inspections_queued: u64,
    pub skipped: u64,
    pub issues: Vec<ScanIssue>,
}

/// One scan result plus privacy-preserving monotonic phase aggregates.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ProfiledScanReport {
    pub report: ScanReport,
    pub performance: ScanPerformance,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ScanCompletion {
    Completed,
    Cancelled,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ScanPhase {
    Discovering,
    Completed,
    Cancelled,
}

/// Small, monotonic snapshot emitted while a folder scan is running.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ScanProgress {
    pub session_id: ImportSessionId,
    pub phase: ScanPhase,
    pub files_seen: u64,
    pub supported_files: u64,
    pub inserted: u64,
    pub unchanged: u64,
    pub needs_revalidation: u64,
    pub decode_inspections_queued: u64,
    pub skipped: u64,
    pub issue_count: u64,
}

/// Cheap, cloneable cancellation token shared by a scan and its decode jobs.
#[derive(Debug, Clone, Default)]
pub struct ScanCancellation {
    cancelled: Arc<AtomicBool>,
}

impl ScanCancellation {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    #[must_use]
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

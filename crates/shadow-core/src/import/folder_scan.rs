//! Public start/resume orchestration for folder scans.

use std::path::Path;

use shadow_catalog::{CatalogHandle, CatalogStore};
use shadow_domain::ImportSessionId;

use crate::{
    DecodeInspectionHandle,
    native_path::{decode_location, encode_location},
};

use super::{
    decode_schedule::DecodeScheduler,
    scan_contract::{ProfiledScanReport, ScanCancellation, ScanError, ScanProgress, ScanReport},
    scan_session::{ScanProfiler, absolute_root, now_ms, run_scan_session},
};

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
    scan_folder_controlled(catalog, root, &ScanCancellation::new(), |_| {})
}

/// Recursively scans supported files while collecting aggregate monotonic
/// phase timings.
///
/// # Errors
///
/// Returns the same errors as [`scan_folder`].
pub fn scan_folder_profiled<C: CatalogStore + ?Sized>(
    catalog: &mut C,
    root: &Path,
) -> Result<ProfiledScanReport, ScanError> {
    scan_folder_profiled_controlled(catalog, root, &ScanCancellation::new(), |_| {})
}

/// Scans a folder while publishing progress and honoring cooperative cancellation.
///
/// # Errors
///
/// Returns the same errors as [`scan_folder`]. Cancellation is a successful,
/// journaled terminal outcome in [`ScanReport`].
pub fn scan_folder_controlled<C: CatalogStore + ?Sized>(
    catalog: &mut C,
    root: &Path,
    cancellation: &ScanCancellation,
    mut progress: impl FnMut(&ScanProgress),
) -> Result<ScanReport, ScanError> {
    let root = absolute_root(root)?;
    let now_ms = now_ms();
    let session_id = catalog.begin_import_session(&encode_location(&root), now_ms)?;
    let mut profiler = ScanProfiler::disabled();
    run_scan_session(
        catalog,
        session_id,
        &root,
        None,
        cancellation,
        &mut progress,
        &mut profiler,
    )
}

/// Scans with cooperative cancellation, progress snapshots, and explicit
/// monotonic phase profiling.
///
/// This is the only plain-scanner path that performs per-file clock reads.
///
/// # Errors
///
/// Returns the same errors as [`scan_folder_controlled`].
pub fn scan_folder_profiled_controlled<C: CatalogStore + ?Sized>(
    catalog: &mut C,
    root: &Path,
    cancellation: &ScanCancellation,
    mut progress: impl FnMut(&ScanProgress),
) -> Result<ProfiledScanReport, ScanError> {
    let root = absolute_root(root)?;
    let now_ms = now_ms();
    let session_id = catalog.begin_import_session(&encode_location(&root), now_ms)?;
    let mut profiler = ScanProfiler::enabled();
    let report = run_scan_session(
        catalog,
        session_id,
        &root,
        None,
        cancellation,
        &mut progress,
        &mut profiler,
    )?;
    Ok(ProfiledScanReport {
        report,
        performance: profiler.into_performance(),
    })
}

/// Scans a folder and schedules missing source-inspection snapshots on a
/// background worker.
///
/// Existing current snapshots for the worker's provider are skipped. The
/// scheduler submits original RAW files by default and original raster files
/// only when the chosen inspector explicitly opts in to their file extension.
/// The bounded worker queue applies backpressure without running decoder code
/// on the scanner or catalog writer threads.
///
/// # Errors
///
/// Returns [`ScanError`] for root resolution, catalog failure, or a stopped
/// inspection worker.
pub fn scan_folder_with_inspection(
    catalog: &mut CatalogHandle,
    inspections: &DecodeInspectionHandle,
    root: &Path,
) -> Result<ScanReport, ScanError> {
    scan_folder_with_inspection_controlled(
        catalog,
        inspections,
        root,
        &ScanCancellation::new(),
        |_| {},
    )
}

/// Scans and schedules decode reconciliation while collecting aggregate
/// scanner-thread timings.
///
/// Pair this with a profiled [`crate::DecodeInspectionActor`] to obtain decode
/// and technical-observation worker timings as well.
///
/// # Errors
///
/// Returns the same errors as [`scan_folder_with_inspection`].
pub fn scan_folder_with_inspection_profiled(
    catalog: &mut CatalogHandle,
    inspections: &DecodeInspectionHandle,
    root: &Path,
) -> Result<ProfiledScanReport, ScanError> {
    scan_folder_with_inspection_profiled_controlled(
        catalog,
        inspections,
        root,
        &ScanCancellation::new(),
        |_| {},
    )
}

/// Scans with decode reconciliation, progress snapshots, and shared cancellation.
///
/// # Errors
///
/// Returns the same errors as [`scan_folder_with_inspection`]. Cancellation is
/// returned as a successful partial report.
pub fn scan_folder_with_inspection_controlled(
    catalog: &mut CatalogHandle,
    inspections: &DecodeInspectionHandle,
    root: &Path,
    cancellation: &ScanCancellation,
    mut progress: impl FnMut(&ScanProgress),
) -> Result<ScanReport, ScanError> {
    let root = absolute_root(root)?;
    let session_id = catalog.begin_import_session(&encode_location(&root), now_ms())?;
    let scheduler = DecodeScheduler {
        catalog: catalog.clone(),
        inspections,
    };
    let mut profiler = ScanProfiler::disabled();
    run_scan_session(
        catalog,
        session_id,
        &root,
        Some(&scheduler),
        cancellation,
        &mut progress,
        &mut profiler,
    )
}

/// Scans with source-inspection reconciliation, cancellation, progress, and
/// explicit scanner-thread profiling.
///
/// # Errors
///
/// Returns the same errors as [`scan_folder_with_inspection_controlled`].
pub fn scan_folder_with_inspection_profiled_controlled(
    catalog: &mut CatalogHandle,
    inspections: &DecodeInspectionHandle,
    root: &Path,
    cancellation: &ScanCancellation,
    mut progress: impl FnMut(&ScanProgress),
) -> Result<ProfiledScanReport, ScanError> {
    let root = absolute_root(root)?;
    let session_id = catalog.begin_import_session(&encode_location(&root), now_ms())?;
    let scheduler = DecodeScheduler {
        catalog: catalog.clone(),
        inspections,
    };
    let mut profiler = ScanProfiler::enabled();
    let report = run_scan_session(
        catalog,
        session_id,
        &root,
        Some(&scheduler),
        cancellation,
        &mut progress,
        &mut profiler,
    )?;
    Ok(ProfiledScanReport {
        report,
        performance: profiler.into_performance(),
    })
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
    resume_scan_controlled(catalog, session_id, &ScanCancellation::new(), |_| {})
}

/// Resumes a scan with cooperative cancellation and progress snapshots.
///
/// # Errors
///
/// Returns the same errors as [`resume_scan`].
pub fn resume_scan_controlled<C: CatalogStore + ?Sized>(
    catalog: &mut C,
    session_id: ImportSessionId,
    cancellation: &ScanCancellation,
    mut progress: impl FnMut(&ScanProgress),
) -> Result<ScanReport, ScanError> {
    let session = catalog.resume_import_session(session_id, now_ms())?;
    let root = decode_location(&session.root)?;
    let mut profiler = ScanProfiler::disabled();
    run_scan_session(
        catalog,
        session_id,
        &root,
        None,
        cancellation,
        &mut progress,
        &mut profiler,
    )
}

/// Resumes an import session while reconciling missing provider snapshots.
///
/// # Errors
///
/// Returns [`ScanError`] under the same conditions as [`resume_scan`] plus an
/// unavailable inspection worker.
pub fn resume_scan_with_inspection(
    catalog: &mut CatalogHandle,
    inspections: &DecodeInspectionHandle,
    session_id: ImportSessionId,
) -> Result<ScanReport, ScanError> {
    resume_scan_with_inspection_controlled(
        catalog,
        inspections,
        session_id,
        &ScanCancellation::new(),
        |_| {},
    )
}

/// Resumes a scan with decode reconciliation, progress, and cancellation.
///
/// # Errors
///
/// Returns the same errors as [`resume_scan_with_inspection`].
pub fn resume_scan_with_inspection_controlled(
    catalog: &mut CatalogHandle,
    inspections: &DecodeInspectionHandle,
    session_id: ImportSessionId,
    cancellation: &ScanCancellation,
    mut progress: impl FnMut(&ScanProgress),
) -> Result<ScanReport, ScanError> {
    let session = catalog.resume_import_session(session_id, now_ms())?;
    let root = decode_location(&session.root)?;
    let scheduler = DecodeScheduler {
        catalog: catalog.clone(),
        inspections,
    };
    let mut profiler = ScanProfiler::disabled();
    run_scan_session(
        catalog,
        session_id,
        &root,
        Some(&scheduler),
        cancellation,
        &mut progress,
        &mut profiler,
    )
}

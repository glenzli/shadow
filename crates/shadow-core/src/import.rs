use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Instant, SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{
    CatalogError, CatalogHandle, CatalogStore, ImportSessionState, RegisterAsset, RegisteredAsset,
    RegistrationStatus, RepresentationFingerprint,
};
use shadow_domain::{ImportSessionId, RepresentationKind};
use thiserror::Error;

use crate::native_path::{NativePathError, decode_location, encode_location};
use crate::performance::{ScanPerformance, measure_if};
use crate::{DecodeInspectionError, DecodeInspectionHandle, DecodeInspectionRequest};

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

#[derive(Debug)]
struct ScanProfiler {
    performance: ScanPerformance,
    started_at: Option<Instant>,
}

impl ScanProfiler {
    fn disabled() -> Self {
        Self {
            performance: ScanPerformance::default(),
            started_at: None,
        }
    }

    fn enabled() -> Self {
        Self {
            performance: ScanPerformance {
                profiled: true,
                ..ScanPerformance::default()
            },
            started_at: Some(Instant::now()),
        }
    }

    fn mark_first_catalogued(&mut self) {
        if self.performance.profiled
            && self.performance.first_catalogued_ms.is_none()
            && let Some(started_at) = self.started_at
        {
            self.performance.first_catalogued_ms = Some(duration_ms(started_at.elapsed()));
        }
    }

    fn finish(&mut self) {
        if let Some(started_at) = self.started_at {
            self.performance.enumeration_total_ms = duration_ms(started_at.elapsed());
        }
    }

    fn into_performance(self) -> ScanPerformance {
        self.performance
    }
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

fn run_scan_session(
    catalog: &mut (impl CatalogStore + ?Sized),
    session_id: ImportSessionId,
    root: &Path,
    scheduler: Option<&DecodeScheduler<'_>>,
    cancellation: &ScanCancellation,
    progress: &mut dyn FnMut(&ScanProgress),
    profiler: &mut ScanProfiler,
) -> Result<ScanReport, ScanError> {
    let mut report = ScanReport {
        session_id,
        completion: ScanCompletion::Completed,
        files_seen: 0,
        supported_files: 0,
        inserted: 0,
        unchanged: 0,
        needs_revalidation: 0,
        decode_inspections_queued: 0,
        skipped: 0,
        issues: Vec::new(),
    };
    publish_progress(&report, ScanPhase::Discovering, progress, profiler);
    let scan_result = {
        let mut runtime = ScanRuntime {
            session_id,
            scheduler,
            cancellation,
            progress,
            profiler,
        };
        scan_directory(catalog, root, &mut runtime, &mut report)
    };
    if let Err(error) = scan_result {
        let message = error.to_string();
        let _ = catalog.finish_import_session(
            session_id,
            ImportSessionState::Failed,
            Some(&message),
            now_ms(),
        );
        return Err(error);
    }
    if cancellation.is_cancelled() {
        report.completion = ScanCompletion::Cancelled;
        catalog.finish_import_session(session_id, ImportSessionState::Cancelled, None, now_ms())?;
        publish_progress(&report, ScanPhase::Cancelled, progress, profiler);
    } else {
        catalog.finish_import_session(session_id, ImportSessionState::Completed, None, now_ms())?;
        publish_progress(&report, ScanPhase::Completed, progress, profiler);
    }
    profiler.finish();
    Ok(report)
}

struct ScanRuntime<'a, 'b> {
    session_id: ImportSessionId,
    scheduler: Option<&'a DecodeScheduler<'a>>,
    cancellation: &'a ScanCancellation,
    progress: &'b mut dyn FnMut(&ScanProgress),
    profiler: &'b mut ScanProfiler,
}

fn scan_directory(
    catalog: &mut (impl CatalogStore + ?Sized),
    directory: &Path,
    runtime: &mut ScanRuntime<'_, '_>,
    report: &mut ScanReport,
) -> Result<(), ScanError> {
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }
    let entries = match measure_if(
        runtime.profiler.performance.profiled,
        &mut runtime.profiler.performance.discovery_io,
        || fs::read_dir(directory),
    ) {
        Ok(entries) => entries,
        Err(error) => {
            record_issue(catalog, directory, error.to_string(), runtime, report)?;
            return Ok(());
        }
    };

    let mut entries = entries;
    loop {
        let next_entry = measure_if(
            runtime.profiler.performance.profiled,
            &mut runtime.profiler.performance.discovery_io,
            || entries.next(),
        );
        let Some(entry) = next_entry else {
            break;
        };
        if runtime.cancellation.is_cancelled() {
            return Ok(());
        }
        let entry = match entry {
            Ok(entry) => entry,
            Err(error) => {
                record_issue(catalog, directory, error.to_string(), runtime, report)?;
                continue;
            }
        };
        if runtime.cancellation.is_cancelled() {
            return Ok(());
        }
        let path = entry.path();
        let file_type = match measure_if(
            runtime.profiler.performance.profiled,
            &mut runtime.profiler.performance.discovery_io,
            || entry.file_type(),
        ) {
            Ok(file_type) => file_type,
            Err(error) => {
                record_issue(catalog, &path, error.to_string(), runtime, report)?;
                continue;
            }
        };

        if file_type.is_symlink() {
            report.skipped += 1;
            publish_progress(
                report,
                ScanPhase::Discovering,
                runtime.progress,
                runtime.profiler,
            );
            continue;
        }
        if file_type.is_dir() {
            scan_directory(catalog, &path, runtime, report)?;
            continue;
        }
        if !file_type.is_file() {
            report.skipped += 1;
            publish_progress(
                report,
                ScanPhase::Discovering,
                runtime.progress,
                runtime.profiler,
            );
            continue;
        }
        scan_file(catalog, &entry, &path, runtime, report)?;
    }

    Ok(())
}

fn scan_file(
    catalog: &mut (impl CatalogStore + ?Sized),
    entry: &fs::DirEntry,
    path: &Path,
    runtime: &mut ScanRuntime<'_, '_>,
    report: &mut ScanReport,
) -> Result<(), ScanError> {
    report.files_seen += 1;
    publish_progress(
        report,
        ScanPhase::Discovering,
        runtime.progress,
        runtime.profiler,
    );
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }
    let Some(kind) = representation_kind(path) else {
        report.skipped += 1;
        publish_progress(
            report,
            ScanPhase::Discovering,
            runtime.progress,
            runtime.profiler,
        );
        return Ok(());
    };
    report.supported_files += 1;
    publish_progress(
        report,
        ScanPhase::Discovering,
        runtime.progress,
        runtime.profiler,
    );
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }

    let metadata = match measure_if(
        runtime.profiler.performance.profiled,
        &mut runtime.profiler.performance.metadata_stat,
        || entry.metadata(),
    ) {
        Ok(metadata) => metadata,
        Err(error) => {
            record_issue(catalog, path, error.to_string(), runtime, report)?;
            return Ok(());
        }
    };
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }
    let request = RegisterAsset {
        kind,
        location: encode_location(path),
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
        now_ms: system_time_ms(SystemTime::now()).unwrap_or_default(),
    };
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }
    measure_if(
        runtime.profiler.performance.profiled,
        &mut runtime.profiler.performance.discovered_journal,
        || catalog.record_import_discovered(runtime.session_id, &request),
    )?;
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }
    let registered = measure_if(
        runtime.profiler.performance.profiled,
        &mut runtime.profiler.performance.asset_registration,
        || catalog.register_import_asset(runtime.session_id, &request),
    )?;
    runtime.profiler.mark_first_catalogued();
    record_registration_progress(
        registered.status,
        runtime.progress,
        runtime.profiler,
        report,
    );
    if runtime.cancellation.is_cancelled() {
        return Ok(());
    }
    if let Some(scheduler) = runtime.scheduler
        && scheduler.schedule(
            path,
            kind,
            &request,
            registered,
            runtime.cancellation,
            runtime.profiler,
        )?
    {
        report.decode_inspections_queued += 1;
        publish_progress(
            report,
            ScanPhase::Discovering,
            runtime.progress,
            runtime.profiler,
        );
    }
    Ok(())
}

fn record_registration_progress(
    status: RegistrationStatus,
    progress: &mut dyn FnMut(&ScanProgress),
    profiler: &mut ScanProfiler,
    report: &mut ScanReport,
) {
    match status {
        RegistrationStatus::Inserted => report.inserted += 1,
        RegistrationStatus::Unchanged => report.unchanged += 1,
        RegistrationStatus::NeedsRevalidation => report.needs_revalidation += 1,
    }
    publish_progress(report, ScanPhase::Discovering, progress, profiler);
}

#[derive(Debug)]
struct DecodeScheduler<'a> {
    catalog: CatalogHandle,
    inspections: &'a DecodeInspectionHandle,
}

impl DecodeScheduler<'_> {
    fn schedule(
        &self,
        path: &Path,
        kind: RepresentationKind,
        request: &RegisterAsset,
        registered: RegisteredAsset,
        cancellation: &ScanCancellation,
        profiler: &mut ScanProfiler,
    ) -> Result<bool, ScanError> {
        if cancellation.is_cancelled() {
            return Ok(false);
        }
        // The scanner only discovers user-owned original files. A provider
        // must explicitly opt into each raster extension; LibRaw and anonymous
        // legacy inspectors remain RAW-only, while a JPEG-only provider never
        // receives TIFF/PNG/HEIF just because those files were imported.
        if !matches!(
            kind,
            RepresentationKind::OriginalRaw | RepresentationKind::OriginalRaster
        ) || !self.inspections.supports_source(kind, path)
            || registered.status == RegistrationStatus::NeedsRevalidation
        {
            return Ok(false);
        }
        let source = RepresentationFingerprint {
            byte_len: request.byte_len,
            modified_at_ms: request.modified_at_ms,
        };
        if measure_if(
            profiler.performance.profiled,
            &mut profiler.performance.decode_current_query,
            || {
                self.catalog.is_decode_output_current(
                    registered.representation_id,
                    self.inspections.provider_id(),
                    self.inspections.provider_version(),
                    source,
                    self.inspections.caches_previews(),
                    self.inspections.proxy_variant_key(),
                    self.inspections.technical_preprocessing_version(),
                )
            },
        )? {
            return Ok(false);
        }
        if cancellation.is_cancelled() {
            return Ok(false);
        }
        let submission = measure_if(
            profiler.performance.profiled,
            &mut profiler.performance.decode_submit_wait,
            || {
                self.inspections.submit_with_cancellation_observed(
                    DecodeInspectionRequest {
                        representation_id: registered.representation_id,
                        path: path.to_path_buf(),
                        expected_source: source,
                    },
                    cancellation,
                )
            },
        )?;
        profiler.performance.decode_queue_full_events = profiler
            .performance
            .decode_queue_full_events
            .saturating_add(submission.queue_full_events);
        drop(submission.ticket);
        Ok(true)
    }
}

fn absolute_root(root: &Path) -> Result<PathBuf, ScanError> {
    if root.is_absolute() {
        Ok(root.to_path_buf())
    } else {
        Ok(std::env::current_dir()
            .map_err(ScanError::CurrentDirectory)?
            .join(root))
    }
}

fn record_issue(
    catalog: &mut (impl CatalogStore + ?Sized),
    path: &Path,
    message: String,
    runtime: &mut ScanRuntime<'_, '_>,
    report: &mut ScanReport,
) -> Result<(), ScanError> {
    catalog.record_import_issue(
        runtime.session_id,
        &encode_location(path),
        &message,
        now_ms(),
    )?;
    report.issues.push(ScanIssue {
        path: path.to_path_buf(),
        message,
    });
    publish_progress(
        report,
        ScanPhase::Discovering,
        runtime.progress,
        runtime.profiler,
    );
    Ok(())
}

fn publish_progress(
    report: &ScanReport,
    phase: ScanPhase,
    progress: &mut dyn FnMut(&ScanProgress),
    profiler: &mut ScanProfiler,
) {
    measure_if(
        profiler.performance.profiled,
        &mut profiler.performance.progress_callback,
        || {
            progress(&ScanProgress {
                session_id: report.session_id,
                phase,
                files_seen: report.files_seen,
                supported_files: report.supported_files,
                inserted: report.inserted,
                unchanged: report.unchanged,
                needs_revalidation: report.needs_revalidation,
                decode_inspections_queued: report.decode_inspections_queued,
                skipped: report.skipped,
                issue_count: u64::try_from(report.issues.len()).unwrap_or(u64::MAX),
            });
        },
    );
}

fn duration_ms(duration: std::time::Duration) -> u64 {
    u64::try_from(duration.as_millis()).unwrap_or(u64::MAX)
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
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };

    use super::*;
    use crate::{DecodeInspectionActor, DecodeInspector};
    use shadow_catalog::{Catalog, CatalogActor};
    use shadow_domain::{
        DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot, EntityId,
        ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PhotoId, RawMetadataSnapshot,
    };

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
    fn controlled_scan_progress_is_monotonic_and_terminal() {
        let root = std::env::temp_dir().join(format!("shadow-progress-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
        fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");
        fs::write(root.join("notes.txt"), b"ignore").expect("write ignored fixture");

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let mut progress = Vec::new();
        let report =
            scan_folder_controlled(&mut catalog, &root, &ScanCancellation::new(), |snapshot| {
                progress.push(snapshot.clone());
            })
            .expect("controlled scan");

        assert_eq!(report.completion, ScanCompletion::Completed);
        assert_eq!(
            progress.first().expect("initial progress").phase,
            ScanPhase::Discovering
        );
        assert_eq!(
            progress.last().expect("terminal progress").phase,
            ScanPhase::Completed
        );
        assert_eq!(
            progress.last().expect("terminal progress").files_seen,
            report.files_seen
        );
        for pair in progress.windows(2) {
            let [before, after] = pair else {
                unreachable!("windows of two always contain two elements")
            };
            assert!(before.files_seen <= after.files_seen);
            assert!(before.supported_files <= after.supported_files);
            assert!(before.inserted <= after.inserted);
            assert!(before.unchanged <= after.unchanged);
            assert!(before.needs_revalidation <= after.needs_revalidation);
            assert!(before.decode_inspections_queued <= after.decode_inspections_queued);
            assert!(before.skipped <= after.skipped);
            assert!(before.issue_count <= after.issue_count);
        }

        fs::remove_dir_all(&root).expect("remove fixture directory");
    }

    #[test]
    fn profiled_scan_routes_scanner_operations_without_per_file_samples() {
        let root = std::env::temp_dir().join(format!("shadow-profiled-scan-{}", PhotoId::new_v7()));
        let nested = root.join("nested");
        fs::create_dir_all(&nested).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
        fs::write(nested.join("two.jpg"), b"jpeg").expect("write raster fixture");
        fs::write(root.join("notes.txt"), b"ignore").expect("write ignored fixture");

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let callback_count = Arc::new(AtomicUsize::new(0));
        let observed_callbacks = Arc::clone(&callback_count);
        let profiled = scan_folder_profiled_controlled(
            &mut catalog,
            &root,
            &ScanCancellation::new(),
            move |_| {
                observed_callbacks.fetch_add(1, Ordering::SeqCst);
            },
        )
        .expect("profile scan");

        assert_eq!(profiled.report.supported_files, 2);
        assert!(profiled.performance.profiled);
        assert_eq!(profiled.performance.metadata_stat.samples, 2);
        assert_eq!(profiled.performance.discovered_journal.samples, 2);
        assert_eq!(profiled.performance.asset_registration.samples, 2);
        assert_eq!(profiled.performance.decode_current_query.samples, 0);
        assert_eq!(profiled.performance.decode_submit_wait.samples, 0);
        assert_eq!(profiled.performance.decode_queue_full_events, 0);
        assert_eq!(
            profiled.performance.progress_callback.samples,
            u64::try_from(callback_count.load(Ordering::SeqCst)).expect("callback count fits u64")
        );
        assert!(profiled.performance.discovery_io.samples >= 2);
        assert!(profiled.performance.first_catalogued_ms.is_some());

        fs::remove_dir_all(&root).expect("remove fixture directory");
    }

    #[test]
    fn profiled_scan_and_actor_keep_scanner_and_worker_phases_separate() {
        let test_id = PhotoId::new_v7();
        let root = std::env::temp_dir().join(format!("shadow-profiled-actor-{test_id}"));
        let database_path =
            std::env::temp_dir().join(format!("shadow-profiled-actor-{test_id}.sqlite"));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
        fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");

        let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
        let mut catalog = actor.handle();
        let worker = DecodeInspectionActor::spawn_profiled(catalog.clone(), |_path: &Path| {
            Ok(scheduled_snapshot())
        })
        .expect("spawn profiled decode worker");
        let scan = scan_folder_with_inspection_profiled(&mut catalog, &worker.handle(), &root)
            .expect("profile scan and scheduling");
        let terminal = worker
            .shutdown_with_performance()
            .expect("drain profiled worker");

        assert_eq!(scan.report.decode_inspections_queued, 1);
        assert_eq!(scan.performance.decode_current_query.samples, 1);
        assert_eq!(scan.performance.decode_submit_wait.samples, 1);
        assert_eq!(terminal.summary.completed, 1);
        assert!(terminal.decode.profiled);
        assert_eq!(terminal.decode.queue_wait.samples, 1);
        assert_eq!(terminal.decode.provider_inspect.samples, 1);
        assert_eq!(terminal.decode.snapshot_catalog_commit.samples, 1);
        assert_eq!(terminal.decode.embedded_preview_extract.samples, 0);
        assert_eq!(terminal.decode.proxy_render.samples, 0);
        assert!(!terminal.technical.profiled);

        actor.shutdown().expect("shutdown catalog");
        fs::remove_dir_all(&root).expect("remove fixture directory");
        fs::remove_file(&database_path).expect("remove test catalog");
        for extension in ["sqlite-wal", "sqlite-shm"] {
            let sidecar = database_path.with_extension(extension);
            if sidecar.exists() {
                fs::remove_file(sidecar).expect("remove catalog sidecar");
            }
        }
    }

    #[test]
    fn cancellation_is_journaled_before_registration_and_a_new_scan_is_idempotent() {
        let root = std::env::temp_dir().join(format!("shadow-cancel-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let cancellation = ScanCancellation::new();
        let callback_token = cancellation.clone();
        let mut progress = Vec::new();
        let cancelled = scan_folder_controlled(&mut catalog, &root, &cancellation, |snapshot| {
            progress.push(snapshot.clone());
            if snapshot.supported_files == 1 {
                callback_token.cancel();
            }
        })
        .expect("cancel scan");

        assert_eq!(cancelled.completion, ScanCompletion::Cancelled);
        assert_eq!(cancelled.inserted, 0);
        assert_eq!(catalog.stats().expect("catalog stats").photos, 0);
        assert_eq!(
            progress.last().expect("terminal progress").phase,
            ScanPhase::Cancelled
        );
        assert_eq!(
            catalog
                .import_session_summary(cancelled.session_id)
                .expect("cancelled session")
                .session
                .state,
            ImportSessionState::Cancelled
        );

        let completed = scan_folder(&mut catalog, &root).expect("fresh scan after cancellation");
        assert_eq!(completed.completion, ScanCompletion::Completed);
        assert_eq!(completed.inserted, 1);
        let repeated = scan_folder(&mut catalog, &root).expect("repeat completed scan");
        assert_eq!(repeated.inserted, 0);
        assert_eq!(repeated.unchanged, 1);
        assert_eq!(catalog.stats().expect("catalog stats").photos, 1);

        fs::remove_dir_all(&root).expect("remove fixture directory");
    }

    #[test]
    fn cancellation_after_one_registration_stops_before_the_next_asset() {
        let root = std::env::temp_dir().join(format!("shadow-partial-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw one").expect("write first fixture");
        fs::write(root.join("two.NEF"), b"raw two").expect("write second fixture");

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let cancellation = ScanCancellation::new();
        let callback_token = cancellation.clone();
        let cancelled = scan_folder_controlled(&mut catalog, &root, &cancellation, |snapshot| {
            if snapshot.inserted == 1 {
                callback_token.cancel();
            }
        })
        .expect("partial scan");
        assert_eq!(cancelled.completion, ScanCompletion::Cancelled);
        assert_eq!(cancelled.inserted, 1);

        let completed = scan_folder(&mut catalog, &root).expect("new idempotent scan");
        assert_eq!(completed.completion, ScanCompletion::Completed);
        assert_eq!(completed.inserted, 1);
        assert_eq!(completed.unchanged, 1);

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

    #[test]
    fn scan_reconciles_only_missing_provider_snapshots() {
        let test_id = PhotoId::new_v7();
        let root = std::env::temp_dir().join(format!("shadow-scheduled-scan-{test_id}"));
        let database_path = std::env::temp_dir().join(format!("shadow-scheduled-{test_id}.sqlite"));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
        fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");

        let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
        let mut catalog = actor.handle();
        let calls = Arc::new(AtomicUsize::new(0));
        let worker_calls = Arc::clone(&calls);
        let worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
            worker_calls.fetch_add(1, Ordering::SeqCst);
            Ok(scheduled_snapshot())
        })
        .expect("spawn decode worker");
        assert!(
            worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaw, Path::new("one.NEF"))
        );
        assert!(
            !worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaster, Path::new("two.jpg"))
        );
        let first = scan_folder_with_inspection(&mut catalog, &worker.handle(), &root)
            .expect("scan and schedule");
        assert_eq!(first.decode_inspections_queued, 1);
        worker.shutdown().expect("drain first decode worker");

        let second_worker_calls = Arc::clone(&calls);
        let second_worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
            second_worker_calls.fetch_add(1, Ordering::SeqCst);
            Ok(scheduled_snapshot())
        })
        .expect("spawn second decode worker");
        let second = scan_folder_with_inspection(&mut catalog, &second_worker.handle(), &root)
            .expect("rescan and reconcile");
        assert_eq!(second.decode_inspections_queued, 0);
        second_worker.shutdown().expect("shutdown second worker");
        assert_eq!(calls.load(Ordering::SeqCst), 1);

        actor.shutdown().expect("shutdown catalog");
        fs::remove_dir_all(&root).expect("remove fixture directory");
        fs::remove_file(&database_path).expect("remove test catalog");
    }

    #[derive(Debug)]
    struct RasterOnlyInspector {
        calls: Arc<AtomicUsize>,
    }

    impl DecodeInspector for RasterOnlyInspector {
        fn provider_id(&self) -> &'static str {
            "raster-fixture"
        }

        fn supports_original_raw(&self) -> bool {
            false
        }

        fn supported_original_raster_extensions(&self) -> Vec<String> {
            vec!["jpg".to_owned(), "jpeg".to_owned()]
        }

        fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
            assert_eq!(
                path.extension().and_then(std::ffi::OsStr::to_str),
                Some("jpg"),
                "the scheduler must not submit RAW to a raster-only inspector"
            );
            self.calls.fetch_add(1, Ordering::SeqCst);
            let mut snapshot = scheduled_snapshot();
            snapshot.provider.id = self.provider_id().to_owned();
            Ok(snapshot)
        }
    }

    #[test]
    fn jpeg_capable_inspector_schedules_jpeg_without_sending_raw_or_other_rasters() {
        let test_id = PhotoId::new_v7();
        let root = std::env::temp_dir().join(format!("shadow-raster-scan-{test_id}"));
        let database_path = std::env::temp_dir().join(format!("shadow-raster-{test_id}.sqlite"));
        fs::create_dir_all(&root).expect("create fixture directory");
        fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
        fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");
        fs::write(root.join("three.png"), b"png").expect("write unsupported raster fixture");
        fs::write(root.join("four.tiff"), b"tiff").expect("write unsupported raster fixture");
        fs::write(root.join("five.heic"), b"heif").expect("write unavailable HEIF fixture");

        let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
        let mut catalog = actor.handle();
        let calls = Arc::new(AtomicUsize::new(0));
        let worker = DecodeInspectionActor::spawn(
            catalog.clone(),
            RasterOnlyInspector {
                calls: Arc::clone(&calls),
            },
        )
        .expect("spawn raster inspector");
        assert!(
            !worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaw, Path::new("one.NEF"))
        );
        assert!(
            worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaster, Path::new("two.jpg"))
        );
        assert!(
            !worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaster, Path::new("three.png"))
        );
        assert!(
            !worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaster, Path::new("four.tiff"))
        );
        assert!(
            !worker
                .handle()
                .supports_source(RepresentationKind::OriginalRaster, Path::new("five.heic"))
        );

        let report = scan_folder_with_inspection(&mut catalog, &worker.handle(), &root)
            .expect("scan with raster inspection");
        assert_eq!(report.supported_files, 5);
        assert_eq!(report.decode_inspections_queued, 1);
        let summary = worker
            .shutdown_with_summary()
            .expect("drain raster inspector");
        assert_eq!(summary.completed, 1);
        assert_eq!(calls.load(Ordering::SeqCst), 1);
        let page = catalog
            .review_page(None, 16)
            .expect("read inspected sources");
        let raw_representation_id = page
            .items
            .iter()
            .find(|item| item.location.display_path.ends_with("one.NEF"))
            .expect("registered RAW source")
            .representation_id;
        let raster_representation_id = page
            .items
            .iter()
            .find(|item| item.location.display_path.ends_with("two.jpg"))
            .expect("registered raster source")
            .representation_id;
        assert!(
            catalog
                .decode_snapshots(raw_representation_id)
                .expect("read RAW snapshots")
                .is_empty(),
            "a raster-only inspector must not record a RAW snapshot"
        );
        assert_eq!(
            catalog
                .decode_snapshots(raster_representation_id)
                .expect("read raster snapshots")
                .len(),
            1,
            "an opted-in raster inspector must complete the ordinary snapshot path"
        );

        actor.shutdown().expect("shutdown catalog");
        fs::remove_dir_all(&root).expect("remove fixture directory");
        fs::remove_file(&database_path).expect("remove test catalog");
        for extension in ["sqlite-wal", "sqlite-shm"] {
            let sidecar = database_path.with_extension(extension);
            if sidecar.exists() {
                fs::remove_file(sidecar).expect("remove catalog sidecar");
            }
        }
    }

    fn scheduled_snapshot() -> DecoderSnapshot {
        DecoderSnapshot {
            provider: DecodeProviderSnapshot {
                id: "anonymous".into(),
                version: "1".into(),
                dng_sdk: false,
                rawspeed: false,
                jpeg: false,
            },
            metadata: RawMetadataSnapshot {
                make: "Test".into(),
                model: "Fixture".into(),
                normalized_make: "Test".into(),
                normalized_model: "Fixture".into(),
                dng_version: None,
                raw_count: 1,
                raw_dimensions: ImageDimensions {
                    width: 10,
                    height: 10,
                },
                image_dimensions: ImageDimensions {
                    width: 10,
                    height: 10,
                },
                margins: ImageMargins::default(),
                orientation: 0,
                cfa_pattern: "RGGB".into(),
                sensor_colors: 3,
                sensor_bits: 12,
                black_level: 0,
                white_level: 4_095,
                as_shot_neutral: [1.0; 4],
                baseline_exposure: 0.0,
                iso_speed: 0.0,
                exposure_time_seconds: 0.0,
                aperture_f_number: 0.0,
                focal_length_mm: 0.0,
                captured_at_unix_seconds: 0,
                lens_make: String::new(),
                lens_model: String::new(),
                focal_length_35mm: 0.0,
            },
            capabilities: DecodeCapabilitySnapshot {
                metadata: DecodeSupport::Available,
                embedded_previews: DecodeSupport::Unavailable,
                raw_frame: DecodeSupport::Available,
                reference_rgb: DecodeSupport::Unavailable,
                pending_corrections: PendingCorrectionsSnapshot::default(),
                raw_development: Default::default(),
            },
            previews: Vec::new(),
        }
    }
}

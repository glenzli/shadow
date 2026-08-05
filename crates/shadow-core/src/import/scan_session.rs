//! One import-session lifecycle, recursive traversal, progress, and profiling.

use std::{
    fs,
    path::{Path, PathBuf},
    time::{Instant, SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{CatalogStore, ImportSessionState, RegisterAsset, RegistrationStatus};
use shadow_domain::{ImportSessionId, RepresentationKind};

use crate::{
    native_path::encode_location,
    performance::{ScanPerformance, measure_if},
};

use super::{
    companion_grouping::grouping_for_path,
    decode_schedule::DecodeScheduler,
    scan_contract::{
        ScanCancellation, ScanCompletion, ScanError, ScanIssue, ScanPhase, ScanProgress, ScanReport,
    },
};

#[derive(Debug)]
pub(super) struct ScanProfiler {
    performance: ScanPerformance,
    started_at: Option<Instant>,
}

impl ScanProfiler {
    pub(super) fn disabled() -> Self {
        Self {
            performance: ScanPerformance::default(),
            started_at: None,
        }
    }

    pub(super) fn enabled() -> Self {
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

    pub(super) fn measure_decode_current_query<T>(&mut self, operation: impl FnOnce() -> T) -> T {
        measure_if(
            self.performance.profiled,
            &mut self.performance.decode_current_query,
            operation,
        )
    }

    pub(super) fn measure_decode_submit_wait<T>(&mut self, operation: impl FnOnce() -> T) -> T {
        measure_if(
            self.performance.profiled,
            &mut self.performance.decode_submit_wait,
            operation,
        )
    }

    pub(super) fn record_decode_queue_full_events(&mut self, events: u64) {
        self.performance.decode_queue_full_events = self
            .performance
            .decode_queue_full_events
            .saturating_add(events);
    }

    fn finish(&mut self) {
        if let Some(started_at) = self.started_at {
            self.performance.enumeration_total_ms = duration_ms(started_at.elapsed());
        }
    }

    pub(super) fn into_performance(self) -> ScanPerformance {
        self.performance
    }
}

pub(super) fn run_scan_session(
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
            root,
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
    root: &'a Path,
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
    let grouping = grouping_for_path(runtime.root, path, kind)?;
    let registered = measure_if(
        runtime.profiler.performance.profiled,
        &mut runtime.profiler.performance.asset_registration,
        || match grouping.as_ref() {
            Some(grouping) => {
                catalog.register_import_asset_grouped(runtime.session_id, &request, grouping)
            }
            None => catalog.register_import_asset(runtime.session_id, &request),
        },
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

pub(super) fn absolute_root(root: &Path) -> Result<PathBuf, ScanError> {
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

pub(super) fn now_ms() -> i64 {
    system_time_ms(SystemTime::now()).unwrap_or_default()
}

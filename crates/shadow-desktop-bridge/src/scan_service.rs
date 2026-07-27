//! Folder-import coordination for the desktop bridge.
//!
//! The scanner owns cancellation, observable progress and terminal state.  It
//! creates short-lived decoder workers for a single run. Generated Shadow
//! proxies and source facts remain durable Catalog/cache data; camera previews
//! are instead published to the separate session-only preview store.

use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::CatalogHandle;
use shadow_core::{
    DecodeInspectionHandle, DecodeInspectionPool, DecodeInspectionProgress,
    DecodeInspectionSummary, EmbeddedPreviewSink, ScanCancellation, ScanCompletion, ScanPhase,
    ScanProgress, recommended_decode_inspection_worker_count,
    scan_folder_with_inspection_controlled,
};

use crate::{ffi, photo_provider::PhotoInspector, session_preview_store::SessionPreviewStore};

#[derive(Debug)]
pub(crate) struct ScanService {
    catalog: CatalogHandle,
    cache_root: PathBuf,
    session_previews: Arc<SessionPreviewStore>,
    registry: Arc<Mutex<FolderScanRegistry>>,
}

#[derive(Debug, Default)]
struct FolderScanRegistry {
    current: Option<FolderScanState>,
}

#[derive(Debug)]
struct FolderScanState {
    scan_id: u64,
    update_sequence: u64,
    started: bool,
    phase: ffi::FfiScanPhase,
    files_seen: u64,
    supported_files: u64,
    inserted: u64,
    unchanged: u64,
    needs_revalidation: u64,
    decode_inspections_queued: u64,
    preview_artifacts_published: u64,
    decode_inspections_completed: u64,
    decode_hard_failures: u64,
    preview_failures: u64,
    decode_inspections_cancelled: u64,
    skipped: u64,
    issue_count: u64,
    cancellation: ScanCancellation,
}

impl FolderScanState {
    fn new(scan_id: u64, cancellation: ScanCancellation) -> Self {
        Self {
            scan_id,
            update_sequence: 1,
            started: false,
            phase: ffi::FfiScanPhase::Discovering,
            files_seen: 0,
            supported_files: 0,
            inserted: 0,
            unchanged: 0,
            needs_revalidation: 0,
            decode_inspections_queued: 0,
            preview_artifacts_published: 0,
            decode_inspections_completed: 0,
            decode_hard_failures: 0,
            preview_failures: 0,
            decode_inspections_cancelled: 0,
            skipped: 0,
            issue_count: 0,
            cancellation,
        }
    }

    fn is_active(&self) -> bool {
        matches!(
            self.phase,
            ffi::FfiScanPhase::Discovering
                | ffi::FfiScanPhase::PreparingPreviews
                | ffi::FfiScanPhase::Cancelling
        )
    }

    fn snapshot(&self) -> ffi::FfiScanProgress {
        ffi::FfiScanProgress {
            valid: true,
            scan_id: self.scan_id,
            update_sequence: self.update_sequence,
            phase: self.phase,
            files_seen: self.files_seen,
            supported_files: self.supported_files,
            inserted: self.inserted,
            unchanged: self.unchanged,
            needs_revalidation: self.needs_revalidation,
            decode_inspections_queued: self.decode_inspections_queued,
            preview_artifacts_published: self.preview_artifacts_published,
            decode_inspections_completed: self.decode_inspections_completed,
            decode_hard_failures: self.decode_hard_failures,
            preview_failures: self.preview_failures,
            decode_inspections_cancelled: self.decode_inspections_cancelled,
            skipped: self.skipped,
            issue_count: self.issue_count,
        }
    }
}

/// Keeps import-visible progress moving after source discovery has finished.
///
/// The scanner intentionally does not wait for each decode task.  Once it has
/// scheduled the last task, this small service-owned pump continues publishing
/// the worker pool's visual commits so the QML grid can replace card artwork in
/// place instead of waiting for the terminal scan result.
struct InspectionProgressPump {
    stop: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl InspectionProgressPump {
    fn spawn(
        scan_id: u64,
        registry: Arc<Mutex<FolderScanRegistry>>,
        inspections: DecodeInspectionHandle,
    ) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let stop_for_thread = Arc::clone(&stop);
        let join = thread::Builder::new()
            .name("shadow-import-progress".to_owned())
            .spawn(move || {
                while !stop_for_thread.load(Ordering::Acquire) {
                    publish_inspection_progress(
                        &registry,
                        scan_id,
                        inspections.progress_snapshot(),
                    );
                    thread::sleep(Duration::from_millis(75));
                }
                publish_inspection_progress(&registry, scan_id, inspections.progress_snapshot());
            })
            .ok();
        Self { stop, join }
    }

    fn stop_and_join(mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

fn apply_inspection_progress(
    state: &mut FolderScanState,
    progress: DecodeInspectionProgress,
) -> bool {
    let summary = progress.summary;
    let changed = state.preview_artifacts_published != progress.visual_artifacts_published
        || state.decode_inspections_completed != summary.completed
        || state.decode_hard_failures != summary.hard_failures
        || state.preview_failures != summary.preview_failures
        || state.decode_inspections_cancelled != summary.cancelled;
    if changed {
        state.preview_artifacts_published = progress.visual_artifacts_published;
        state.decode_inspections_completed = summary.completed;
        state.decode_hard_failures = summary.hard_failures;
        state.preview_failures = summary.preview_failures;
        state.decode_inspections_cancelled = summary.cancelled;
    }
    changed
}

fn publish_inspection_progress(
    registry: &Mutex<FolderScanRegistry>,
    scan_id: u64,
    progress: DecodeInspectionProgress,
) {
    let Ok(mut registry) = registry.lock() else {
        return;
    };
    let Some(state) = registry
        .current
        .as_mut()
        .filter(|state| state.scan_id == scan_id && state.is_active())
    else {
        return;
    };
    if apply_inspection_progress(state, progress) {
        state.update_sequence = state.update_sequence.saturating_add(1);
    }
}

impl ScanService {
    pub(crate) fn new(
        catalog: CatalogHandle,
        cache_root: PathBuf,
        session_previews: Arc<SessionPreviewStore>,
    ) -> Self {
        Self {
            catalog,
            cache_root,
            session_previews,
            registry: Arc::new(Mutex::new(FolderScanRegistry::default())),
        }
    }

    pub(crate) fn scan_folder(
        &self,
        folder_path: &str,
        scan_id: u64,
    ) -> AnyResult<ffi::FfiScanReport> {
        let cancellation = self.start_cancellation(scan_id)?;
        let folder_path = Path::new(folder_path);
        let mut catalog = self.catalog.clone();
        let worker_count = recommended_decode_inspection_worker_count();
        let embedded_preview_sink: Arc<dyn EmbeddedPreviewSink> = self.session_previews.clone();
        let inspector = match DecodeInspectionPool::spawn_with_cache_and_embedded_preview_sink(
            catalog.clone(),
            worker_count,
            |_| PhotoInspector::new_with_isolated_proxy_cache(Some(self.cache_root.clone())),
            &self.cache_root,
            embedded_preview_sink,
        ) {
            Ok(inspector) => inspector,
            Err(error) => {
                self.finish_failed(scan_id)?;
                return Err(error.into());
            }
        };
        let inspection_handle = inspector.handle();
        let progress_pump = InspectionProgressPump::spawn(
            scan_id,
            Arc::clone(&self.registry),
            inspection_handle.clone(),
        );
        let report_result = scan_folder_with_inspection_controlled(
            &mut catalog,
            &inspection_handle,
            folder_path,
            &cancellation,
            |progress| {
                let _ =
                    self.update_progress(scan_id, progress, inspection_handle.progress_snapshot());
            },
        )
        .with_context(|| format!("scan {}", folder_path.display()));
        let report = match report_result {
            Ok(report) => {
                let phase = match report.completion {
                    ScanCompletion::Completed => ffi::FfiScanPhase::PreparingPreviews,
                    ScanCompletion::Cancelled => ffi::FfiScanPhase::Cancelling,
                };
                self.update_report(scan_id, &report, phase)?;
                report
            }
            Err(error) => {
                cancellation.cancel();
                let shutdown_result = inspector.shutdown_with_summary();
                progress_pump.stop_and_join();
                self.finish_failed(scan_id)?;
                if let Err(shutdown_error) = shutdown_result {
                    return Err(error.context(format!(
                        "decode inspection shutdown also failed: {shutdown_error}"
                    )));
                }
                return Err(error);
            }
        };
        let summary = match inspector.shutdown_with_summary() {
            Ok(summary) => summary,
            Err(error) => {
                progress_pump.stop_and_join();
                self.finish_failed(scan_id)?;
                return Err(error.into());
            }
        };
        progress_pump.stop_and_join();
        let inspection_progress = inspection_handle.progress_snapshot();
        if let Err(error) =
            validate_decode_inspection_summary(report.decode_inspections_queued, &summary)
        {
            self.finish_failed(scan_id)?;
            return Err(error);
        }
        let cancelled = self.finish(scan_id, &report, &inspection_progress)?;
        Ok(ffi::FfiScanReport {
            folder_path: folder_path.display().to_string(),
            files_seen: report.files_seen,
            supported_files: report.supported_files,
            inserted: report.inserted,
            unchanged: report.unchanged,
            needs_revalidation: report.needs_revalidation,
            decode_inspections_queued: report.decode_inspections_queued,
            decode_inspections_completed: summary.completed,
            decode_hard_failures: summary.hard_failures,
            preview_failures: summary.preview_failures,
            decode_inspections_cancelled: summary.cancelled,
            issue_count: u64::try_from(report.issues.len()).unwrap_or(u64::MAX),
            cancelled,
        })
    }

    pub(crate) fn progress(&self, scan_id: u64) -> AnyResult<ffi::FfiScanProgress> {
        if scan_id == 0 {
            bail!("scan id must be non-zero");
        }
        let registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        Ok(registry
            .current
            .as_ref()
            .filter(|state| state.scan_id == scan_id)
            .map_or_else(|| invalid_progress(scan_id), FolderScanState::snapshot))
    }

    pub(crate) fn cancel(&self, scan_id: u64) -> AnyResult<bool> {
        if scan_id == 0 {
            bail!("scan id must be non-zero");
        }
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        if !state.is_active() {
            return Ok(false);
        }
        let newly_cancelled = !state.cancellation.is_cancelled();
        state.cancellation.cancel();
        if state.phase != ffi::FfiScanPhase::Cancelling {
            state.phase = ffi::FfiScanPhase::Cancelling;
            state.update_sequence = state.update_sequence.saturating_add(1);
        }
        Ok(newly_cancelled)
    }

    pub(crate) fn begin(&self, scan_id: u64) -> AnyResult<()> {
        if scan_id == 0 {
            bail!("scan id must be non-zero");
        }
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        if registry
            .current
            .as_ref()
            .is_some_and(FolderScanState::is_active)
        {
            bail!("another folder scan is already active");
        }
        registry.current = Some(FolderScanState::new(scan_id, ScanCancellation::new()));
        Ok(())
    }

    #[cfg(test)]
    pub(crate) fn cancellation_for_start(&self, scan_id: u64) -> AnyResult<ScanCancellation> {
        self.start_cancellation(scan_id)
    }

    fn start_cancellation(&self, scan_id: u64) -> AnyResult<ScanCancellation> {
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id && state.is_active())
            .ok_or_else(|| anyhow!("scan id {scan_id} was not prepared"))?;
        if state.started {
            bail!("scan id {scan_id} has already started");
        }
        state.started = true;
        Ok(state.cancellation.clone())
    }

    fn update_progress(
        &self,
        scan_id: u64,
        progress: &ScanProgress,
        inspection_progress: DecodeInspectionProgress,
    ) -> AnyResult<()> {
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.files_seen = progress.files_seen;
        state.supported_files = progress.supported_files;
        state.inserted = progress.inserted;
        state.unchanged = progress.unchanged;
        state.needs_revalidation = progress.needs_revalidation;
        state.decode_inspections_queued = progress.decode_inspections_queued;
        apply_inspection_progress(state, inspection_progress);
        state.skipped = progress.skipped;
        state.issue_count = progress.issue_count;
        state.phase = if state.cancellation.is_cancelled() || progress.phase == ScanPhase::Cancelled
        {
            ffi::FfiScanPhase::Cancelling
        } else {
            ffi::FfiScanPhase::Discovering
        };
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(())
    }

    pub(crate) fn update_report(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        phase: ffi::FfiScanPhase,
    ) -> AnyResult<()> {
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.files_seen = report.files_seen;
        state.supported_files = report.supported_files;
        state.inserted = report.inserted;
        state.unchanged = report.unchanged;
        state.needs_revalidation = report.needs_revalidation;
        state.decode_inspections_queued = report.decode_inspections_queued;
        state.skipped = report.skipped;
        state.issue_count = u64::try_from(report.issues.len()).unwrap_or(u64::MAX);
        state.phase =
            if state.cancellation.is_cancelled() && phase == ffi::FfiScanPhase::PreparingPreviews {
                ffi::FfiScanPhase::Cancelling
            } else {
                phase
            };
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(())
    }

    pub(crate) fn finish(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        inspection_progress: &DecodeInspectionProgress,
    ) -> AnyResult<bool> {
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.files_seen = report.files_seen;
        state.supported_files = report.supported_files;
        state.inserted = report.inserted;
        state.unchanged = report.unchanged;
        state.needs_revalidation = report.needs_revalidation;
        state.decode_inspections_queued = report.decode_inspections_queued;
        apply_inspection_progress(state, *inspection_progress);
        state.skipped = report.skipped;
        state.issue_count = u64::try_from(report.issues.len()).unwrap_or(u64::MAX);
        let cancelled =
            report.completion == ScanCompletion::Cancelled || state.cancellation.is_cancelled();
        state.phase = if cancelled {
            ffi::FfiScanPhase::Cancelled
        } else {
            ffi::FfiScanPhase::Completed
        };
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(cancelled)
    }

    fn finish_failed(&self, scan_id: u64) -> AnyResult<()> {
        let mut registry = self
            .registry
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.phase = ffi::FfiScanPhase::Failed;
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(())
    }
}

fn invalid_progress(scan_id: u64) -> ffi::FfiScanProgress {
    ffi::FfiScanProgress {
        valid: false,
        scan_id,
        update_sequence: 0,
        phase: ffi::FfiScanPhase::Idle,
        files_seen: 0,
        supported_files: 0,
        inserted: 0,
        unchanged: 0,
        needs_revalidation: 0,
        decode_inspections_queued: 0,
        preview_artifacts_published: 0,
        decode_inspections_completed: 0,
        decode_hard_failures: 0,
        preview_failures: 0,
        decode_inspections_cancelled: 0,
        skipped: 0,
        issue_count: 0,
    }
}

pub(crate) fn validate_decode_inspection_summary(
    queued: u64,
    summary: &DecodeInspectionSummary,
) -> AnyResult<()> {
    if summary.completed > queued {
        bail!(
            "decode worker completed {} jobs after only {queued} were queued",
            summary.completed
        );
    }
    let diagnostic_jobs = summary
        .hard_failures
        .saturating_add(summary.preview_failures)
        .saturating_add(summary.cancelled);
    if diagnostic_jobs > summary.completed {
        bail!(
            "decode worker reported {diagnostic_jobs} diagnostic jobs after completing only {}",
            summary.completed
        );
    }
    if summary.completed != queued {
        bail!(
            "decode worker completed {} of {queued} queued jobs",
            summary.completed
        );
    }
    Ok(())
}

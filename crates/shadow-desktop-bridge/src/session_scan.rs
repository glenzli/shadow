//! Desktop-session CXX delegations and test hooks for folder scanning.

use anyhow::Result as AnyResult;
#[cfg(test)]
use shadow_core::{DecodeInspectionSummary, ScanCancellation};

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn scan_folder(
        &self,
        folder_path: &str,
        scan_id: u64,
    ) -> AnyResult<ffi::FfiScanReport> {
        self.scanner.scan_folder(folder_path, scan_id)
    }

    pub(crate) fn scan_progress(&self, scan_id: u64) -> AnyResult<ffi::FfiScanProgress> {
        self.scanner.progress(scan_id)
    }

    pub(crate) fn cancel_folder_scan(&self, scan_id: u64) -> AnyResult<bool> {
        self.scanner.cancel(scan_id)
    }

    pub(crate) fn begin_folder_scan(&self, scan_id: u64) -> AnyResult<()> {
        self.scanner.begin(scan_id)
    }

    #[cfg(test)]
    pub(crate) fn folder_scan_cancellation(&self, scan_id: u64) -> AnyResult<ScanCancellation> {
        self.scanner.cancellation_for_start(scan_id)
    }

    #[cfg(test)]
    pub(crate) fn update_folder_scan_report(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        phase: ffi::FfiScanPhase,
    ) -> AnyResult<()> {
        self.scanner.update_report(scan_id, report, phase)
    }

    #[cfg(test)]
    pub(crate) fn finish_folder_scan(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        summary: &DecodeInspectionSummary,
    ) -> AnyResult<bool> {
        self.scanner.finish(
            scan_id,
            report,
            &shadow_core::DecodeInspectionProgress {
                summary: *summary,
                visual_artifacts_published: 0,
            },
        )
    }
}

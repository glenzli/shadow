//! Desktop-session CXX delegations for durable export queue execution.

use anyhow::Result as AnyResult;

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn enqueue_durable_export_job(
        &self,
        targets: Vec<ffi::FfiDurableExportTarget>,
        settings_json: &str,
    ) -> AnyResult<ffi::FfiDurableExportJob> {
        self.export_queue.enqueue(self, targets, settings_json)
    }

    pub(crate) fn recover_durable_export_queue(&self) -> AnyResult<ffi::FfiDurableExportRecovery> {
        self.export_queue.recover_for_startup()
    }

    pub(crate) fn claim_next_durable_export_item(&self) -> AnyResult<ffi::FfiDurableExportItem> {
        Ok(self
            .export_queue
            .claim_next()?
            .unwrap_or_else(|| ffi::FfiDurableExportItem {
                has_item: false,
                item_id: String::new(),
                job_id: String::new(),
                photo_id: String::new(),
                source_path: String::new(),
                output_path: ffi::FfiNativePath {
                    platform: ffi::FfiNativePathPlatform::MacOs,
                    unix_bytes: Vec::new(),
                    windows_units: Vec::new(),
                    display_path: String::new(),
                },
                settings_json: String::new(),
            }))
    }

    pub(crate) fn begin_durable_export_render(&self, item_id: &str) -> AnyResult<()> {
        self.export_queue.begin_render(item_id)
    }

    pub(crate) fn render_durable_export_item(
        &self,
        item: &ffi::FfiDurableExportItem,
    ) -> AnyResult<ffi::FfiEditedExportRaster> {
        self.export_queue.render(self, item)
    }

    pub(crate) fn begin_durable_export_encoding(&self, item_id: &str) -> AnyResult<()> {
        self.export_queue.begin_encoding(item_id)
    }

    pub(crate) fn begin_durable_export_write(&self, item_id: &str) -> AnyResult<()> {
        self.export_queue.begin_writing(item_id)
    }

    pub(crate) fn pause_durable_export_conflict(&self, item_id: &str) -> AnyResult<()> {
        self.export_queue.pause_for_conflict(item_id)
    }

    pub(crate) fn complete_durable_export_item(
        &self,
        item_id: &str,
        job_id: &str,
        output_format: &str,
        byte_len: u64,
        receipt_json: &str,
    ) -> AnyResult<()> {
        self.export_queue
            .complete(item_id, job_id, output_format, byte_len, receipt_json)
    }

    pub(crate) fn fail_durable_export_item(
        &self,
        item_id: &str,
        stage: ffi::FfiDurableExportItemState,
        code: &str,
        message: &str,
        retryable: bool,
    ) -> AnyResult<()> {
        self.export_queue
            .fail_from_ffi(item_id, stage, code, message, retryable)
    }

    pub(crate) fn cancel_durable_export_job(&self, job_id: &str) -> AnyResult<()> {
        self.export_queue.cancel_job(job_id)
    }

    pub(crate) fn durable_export_progress(
        &self,
        job_id: &str,
    ) -> AnyResult<ffi::FfiDurableExportProgress> {
        self.export_queue.progress(job_id)
    }
}

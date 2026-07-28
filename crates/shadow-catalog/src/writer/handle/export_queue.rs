//! Client adapters for durable export jobs, worker transitions, and recovery.

use crate::{
    AdvanceExportItem, CatalogError, EnqueueExportJob, ExportItemId, ExportItemRecord, ExportJobId,
    ExportJobProgress, ExportJobRecord, ExportOutputReceiptRecord, ExportQueueRecovery,
};

use super::super::{
    CatalogHandle,
    protocol::{ExportQueueMessage, Message},
};

impl CatalogHandle {
    /// Atomically freezes one durable export job and all of its item snapshots.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or snapshots are invalid.
    pub fn enqueue_export_job(
        &self,
        request: &EnqueueExportJob,
    ) -> Result<ExportJobRecord, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::Enqueue(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Resolves one durable export job.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or persisted data is invalid.
    pub fn export_job(&self, job_id: ExportJobId) -> Result<Option<ExportJobRecord>, CatalogError> {
        self.request(|response| Message::ExportQueue(ExportQueueMessage::Job(job_id, response)))
    }

    /// Lists a bounded task-center page of export jobs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an unavailable actor or invalid page bound.
    pub fn export_jobs(&self, limit: usize) -> Result<Vec<ExportJobRecord>, CatalogError> {
        self.request(|response| Message::ExportQueue(ExportQueueMessage::Jobs(limit, response)))
    }

    /// Lists the individual immutable item snapshots for one export job.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the job is absent.
    pub fn export_job_items(
        &self,
        job_id: ExportJobId,
    ) -> Result<Vec<ExportItemRecord>, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::JobItems(job_id, response))
        })
    }

    /// Reads one export item by its primary-key identity without materializing
    /// the rest of its job. Workers use this on retry/completion paths so a
    /// huge batch remains O(1) per item.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or stored item
    /// data is malformed.
    pub fn export_item(
        &self,
        item_id: ExportItemId,
    ) -> Result<Option<ExportItemRecord>, CatalogError> {
        self.request(|response| Message::ExportQueue(ExportQueueMessage::Item(item_id, response)))
    }

    /// Aggregates durable worker state for one job inside `SQLite`, without
    /// moving the job's individual item records through the actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor is unavailable, the job is
    /// absent, or its stored item state is malformed.
    pub fn export_job_progress(
        &self,
        job_id: ExportJobId,
    ) -> Result<ExportJobProgress, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::JobProgress(job_id, response))
        })
    }

    /// Claims the next queued export item, atomically moving it to preparing.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or claiming fails.
    pub fn claim_next_export_item(
        &self,
        now_ms: i64,
    ) -> Result<Option<ExportItemRecord>, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::ClaimNext(now_ms, response))
        })
    }

    /// Advances one export item with a compare-and-swap state transition.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an unavailable actor or stale transition.
    pub fn advance_export_item(
        &self,
        request: &AdvanceExportItem,
    ) -> Result<ExportItemRecord, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::Advance(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Returns the immutable receipt for a completed output item.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or receipt data is invalid.
    pub fn export_output_receipt(
        &self,
        item_id: ExportItemId,
    ) -> Result<Option<ExportOutputReceiptRecord>, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::OutputReceipt(item_id, response))
        })
    }

    /// Cancels unfinished items in one job without deleting completed outputs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the job is absent.
    pub fn cancel_export_job(
        &self,
        job_id: ExportJobId,
        now_ms: i64,
    ) -> Result<ExportJobRecord, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::Cancel(job_id, now_ms, response))
        })
    }

    /// Marks in-progress export work interrupted after a process restart.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or recovery fails.
    pub fn recover_interrupted_export_jobs(&self, now_ms: i64) -> Result<usize, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::RecoverInterrupted(now_ms, response))
        })
    }

    /// Atomically makes every retryable interrupted item available at startup.
    ///
    /// The returned report uses global aggregate counts rather than task-center
    /// pages, so recovery stays complete for very large libraries and export
    /// batches.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or recovery
    /// cannot commit.
    pub fn recover_and_requeue_interrupted_export_items(
        &self,
        now_ms: i64,
    ) -> Result<ExportQueueRecovery, CatalogError> {
        self.request(|response| {
            Message::ExportQueue(ExportQueueMessage::RecoverAndRequeue(now_ms, response))
        })
    }
}

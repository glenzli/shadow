//! Durable export presets, immutable render snapshots, and resumable queue state.
//!
//! Exporting is intentionally a catalog concern rather than a transient UI task.
//! A queued item owns an immutable recipe commit, source identity, render plan,
//! output target, and output-settings snapshot. Renderers may therefore retry or
//! resume work without accidentally exporting a later working edit.
//!
//! Navigate to `presets` for immutable settings history, `job_queue` for
//! enqueue and read projections, and `worker_lifecycle` for resumable execution.
//! Persisted shapes live in `schema`, public contracts in `model`, and stored
//! row integrity in `row_codec`.

mod job_queue;
mod model;
mod presets;
mod row_codec;
mod schema;
mod validation;
mod worker_lifecycle;

pub use model::{
    AdvanceExportItem, EnqueueExportJob, ExportFailure, ExportItemId, ExportItemRecord,
    ExportItemState, ExportJobId, ExportJobProgress, ExportJobRecord, ExportJobState,
    ExportOutputReceiptId, ExportOutputReceiptRecord, ExportPresetId, ExportPresetRecord,
    ExportPresetRevisionId, ExportPresetRevisionRecord, ExportQueueRecovery, ExportSettingsSource,
    MAX_EXPORT_JOB_PAGE_SIZE, NewExportItem, NewExportOutputReceipt,
};
pub(crate) use schema::SCHEMA_EXPORT_QUEUE;

#[cfg(test)]
mod tests;

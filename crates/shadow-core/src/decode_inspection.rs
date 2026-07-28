//! Background decode inspection and preview publication.
//!
//! Navigate by responsibility:
//! - [`contract`] owns provider-neutral inputs, outcomes, errors, and terminal
//!   accounting.
//! - [`pool`] indexes the running worker-group lifecycle and the independent
//!   provider pool factory.
//! - [`submission`] owns bounded admission, source support routing, progress
//!   snapshots, and completion tickets.
//! - [`worker`] owns the guarded inspect-and-record transaction.
//! - [`visual_cache`] owns transient embedded previews and durable generated
//!   proxy publication.
//! - [`source_identity`] owns filesystem fingerprints used at async
//!   boundaries.
//! - [`tests`] verifies admission, worker and pool lifecycle, preview caching,
//!   and transient publication as separate behavioral contracts.
//!
//! The public surface is re-exported here so callers depend on the use-case
//! boundary rather than its internal file layout.

mod contract;
mod pool;
mod runtime_state;
mod source_identity;
mod submission;
mod visual_cache;
mod worker;

pub use contract::{
    DecodeInspectionDiscardReason, DecodeInspectionError, DecodeInspectionOutcome,
    DecodeInspectionProgress, DecodeInspectionRequest, DecodeInspectionSummary,
    DecodeInspectionTerminal, DecodeInspector, EmbeddedPreviewPublication, EmbeddedPreviewSink,
    PreviewCacheOutcome,
};
pub use pool::{
    DecodeInspectionActor, DecodeInspectionPool, recommended_decode_inspection_worker_count,
};
pub use source_identity::fingerprint_source;
pub use submission::{DecodeInspectionHandle, DecodeInspectionTicket};

#[cfg(test)]
use pool::{MAX_RECOMMENDED_INSPECTION_WORKERS, MIN_RECOMMENDED_INSPECTION_WORKERS};

#[cfg(test)]
mod tests;

//! Decode-inspection worker ownership and pool construction.
//!
//! Navigate by lifecycle:
//! - [`actor`] owns the running worker group, provider contract, shutdown, and
//!   performance aggregation;
//! - [`factory`] owns worker-count policy and construction of independent
//!   provider instances.

mod actor;
mod factory;

pub use actor::DecodeInspectionActor;
pub use factory::{DecodeInspectionPool, recommended_decode_inspection_worker_count};

#[cfg(test)]
pub(super) use factory::{MAX_RECOMMENDED_INSPECTION_WORKERS, MIN_RECOMMENDED_INSPECTION_WORKERS};

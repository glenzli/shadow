//! Decoder snapshots and the policy that decides whether downstream output is current.
//!
//! [`snapshot_store`] owns durable provider observations and source guards.
//! [`output_freshness`] owns proxy and technical-backfill completion policy.

mod output_freshness;
mod snapshot_store;

pub(crate) use snapshot_store::representation_fingerprint_in_transaction;
pub use snapshot_store::{
    DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};

#[cfg(test)]
mod output_freshness_tests;
#[cfg(test)]
mod snapshot_store_tests;
#[cfg(test)]
mod test_support;

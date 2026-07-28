//! Shared terminal and publication accounting for the worker pool.

use super::contract::DecodeInspectionSummary;

#[derive(Debug, Default)]
pub(super) struct DecodeInspectionState {
    pub(super) stopping: bool,
    pub(super) worker_panicked: bool,
    pub(super) summary: DecodeInspectionSummary,
    pub(super) visual_artifacts_published: u64,
}

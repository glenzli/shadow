//! Folder import, source-health recovery, and verified relinking.
//!
//! Start with [`scan_folder`] or [`resume_scan`] for the public workflow,
//! [`scan_contract`] for progress and terminal values, [`scan_session`] for the
//! recursive journaled state machine, and [`decode_schedule`] for optional
//! decode-output reconciliation.

mod decode_schedule;
mod folder_scan;
mod scan_contract;
mod scan_session;
mod source_health;
mod source_relink;

pub use folder_scan::{
    resume_scan, resume_scan_controlled, resume_scan_with_inspection,
    resume_scan_with_inspection_controlled, scan_folder, scan_folder_controlled,
    scan_folder_profiled, scan_folder_profiled_controlled, scan_folder_with_inspection,
    scan_folder_with_inspection_controlled, scan_folder_with_inspection_profiled,
    scan_folder_with_inspection_profiled_controlled,
};
pub use scan_contract::{
    ProfiledScanReport, ScanCancellation, ScanCompletion, ScanError, ScanIssue, ScanPhase,
    ScanProgress, ScanReport,
};
pub use source_health::{
    SafeReattachPlan, SafeReattachPlanningError, plan_safe_reattach,
    relink_candidate_from_missing_location,
};
pub use source_relink::{
    CatalogRelinkConfirmation, ConfirmedRelink, PendingStrongRelink, RelinkApplyError,
    RelinkCandidate, RelinkIdentityLookup, RelinkSource, RelinkVerificationError,
    SourceRelinkResolution, StrongRelinkVerification, UnsupportedRelinkSource, VerifiedRelink,
    WeakRelinkEvidence, WeakRelinkMetadata, apply_confirmed_relink, confirm_verified_relink,
    discover_source_relink, verify_pending_relink,
};

#[cfg(test)]
mod tests;

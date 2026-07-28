//! Application use cases shared by the desktop app and developer CLI.

mod cached_artifact_loader;
mod catalog_backup;
mod decode_inspection;
#[cfg(test)]
mod display_jpeg_fixture;
mod import;
mod native_path;
mod performance;
mod technical_observation;
mod work_scheduler;

pub use cached_artifact_loader::{
    CachedArtifactInvalidationReason, CachedArtifactLoadError, CachedArtifactLoader,
};
pub use catalog_backup::{
    CatalogBackupCompletion, CatalogBackupJob, CatalogBackupJobError, CatalogBackupPlan,
    CatalogBackupPolicy, CatalogBackupPolicyError, CatalogBackupRetention,
    CatalogBackupScheduleError, CatalogBackupService,
};
pub use decode_inspection::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionError,
    DecodeInspectionHandle, DecodeInspectionOutcome, DecodeInspectionPool,
    DecodeInspectionProgress, DecodeInspectionRequest, DecodeInspectionSummary,
    DecodeInspectionTerminal, DecodeInspectionTicket, DecodeInspector, EmbeddedPreviewPublication,
    EmbeddedPreviewSink, PreviewCacheOutcome, fingerprint_source,
    recommended_decode_inspection_worker_count,
};
pub use import::{
    CatalogRelinkConfirmation, ConfirmedRelink, PendingStrongRelink, ProfiledScanReport,
    RelinkApplyError, RelinkCandidate, RelinkIdentityLookup, RelinkSource, RelinkVerificationError,
    SafeReattachPlan, SafeReattachPlanningError, ScanCancellation, ScanCompletion, ScanError,
    ScanIssue, ScanPhase, ScanProgress, ScanReport, SourceRelinkResolution,
    StrongRelinkVerification, UnsupportedRelinkSource, VerifiedRelink, WeakRelinkEvidence,
    WeakRelinkMetadata, apply_confirmed_relink, confirm_verified_relink, discover_source_relink,
    plan_safe_reattach, relink_candidate_from_missing_location, resume_scan,
    resume_scan_controlled, resume_scan_with_inspection, resume_scan_with_inspection_controlled,
    scan_folder, scan_folder_controlled, scan_folder_profiled, scan_folder_profiled_controlled,
    scan_folder_with_inspection, scan_folder_with_inspection_controlled,
    scan_folder_with_inspection_profiled, scan_folder_with_inspection_profiled_controlled,
    verify_pending_relink,
};
pub use native_path::{NativePathError, native_location};
pub use performance::{
    DecodePerformance, DurationStats, IMPORT_ENGINE_PERFORMANCE_SCHEMA_VERSION,
    ImportEnginePerformance, ScanPerformance, TechnicalPerformance,
};
pub use technical_observation::{
    TECHNICAL_ANALYSIS_MAX_EDGE, TechnicalObservationActor, TechnicalObservationError,
    TechnicalObservationHandle, TechnicalObservationOutcome, TechnicalObservationTicket,
    technical_analysis_preprocessing_version,
};
pub use work_scheduler::{
    WorkCancellation, WorkContext, WorkDiscardReason, WorkGeneration, WorkGenerationSource,
    WorkOutcome, WorkPriority, WorkScheduler, WorkSchedulerConfig, WorkSchedulerError,
    WorkSchedulerHandle, WorkSchedulerSnapshot, WorkSpec, WorkSubmitError, WorkTicket,
    WorkWaitError,
};

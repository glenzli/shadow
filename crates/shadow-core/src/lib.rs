//! Application use cases shared by the desktop app and developer CLI.

mod cached_artifact_loader;
mod decode_inspection;
mod import;
mod native_path;
mod performance;
mod technical_observation;

pub use cached_artifact_loader::{
    CachedArtifactInvalidationReason, CachedArtifactLoadError, CachedArtifactLoader,
};
pub use decode_inspection::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionError,
    DecodeInspectionHandle, DecodeInspectionOutcome, DecodeInspectionRequest,
    DecodeInspectionSummary, DecodeInspectionTerminal, DecodeInspectionTicket, DecodeInspector,
    PreviewCacheOutcome, fingerprint_source,
};
pub use import::{
    ProfiledScanReport, ScanCancellation, ScanCompletion, ScanIssue, ScanPhase, ScanProgress,
    ScanReport, resume_scan, resume_scan_controlled, resume_scan_with_inspection,
    resume_scan_with_inspection_controlled, scan_folder, scan_folder_controlled,
    scan_folder_profiled, scan_folder_profiled_controlled, scan_folder_with_inspection,
    scan_folder_with_inspection_controlled, scan_folder_with_inspection_profiled,
    scan_folder_with_inspection_profiled_controlled,
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

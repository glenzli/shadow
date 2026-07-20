//! Application use cases shared by the desktop app and developer CLI.

mod cached_artifact_loader;
mod decode_inspection;
mod import;
mod native_path;

pub use cached_artifact_loader::{CachedArtifactLoadError, CachedArtifactLoader};
pub use decode_inspection::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionError,
    DecodeInspectionHandle, DecodeInspectionOutcome, DecodeInspectionRequest,
    DecodeInspectionTicket, DecodeInspector, PreviewCacheOutcome, fingerprint_source,
};
pub use import::{
    ScanIssue, ScanReport, resume_scan, resume_scan_with_inspection, scan_folder,
    scan_folder_with_inspection,
};
pub use native_path::{NativePathError, native_location};

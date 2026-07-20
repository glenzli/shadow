//! Application use cases shared by the desktop app and developer CLI.

mod decode_inspection;
mod import;
mod native_path;

pub use decode_inspection::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionError,
    DecodeInspectionHandle, DecodeInspectionOutcome, DecodeInspectionRequest,
    DecodeInspectionTicket, DecodeInspector, fingerprint_source,
};
pub use import::{ScanIssue, ScanReport, resume_scan, scan_folder};
pub use native_path::{NativePathError, native_location};

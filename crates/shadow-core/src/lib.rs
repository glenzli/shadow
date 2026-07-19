//! Application use cases shared by the desktop app and developer CLI.

mod import;
mod native_path;

pub use import::{ScanIssue, ScanReport, scan_folder};

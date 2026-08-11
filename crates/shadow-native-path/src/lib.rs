//! Lossless platform-native path persistence for [`shadow_domain::AssetLocation`].
//!
//! Windows paths are persisted as little-endian UTF-16 code units rather than
//! Unicode strings. This intentionally preserves unpaired surrogate code units.
//! Unix and macOS paths retain their original `OsStr` bytes. The display path
//! is descriptive only and never participates in filesystem access.

mod host;
mod windows;

use thiserror::Error;

pub use host::{current_platform, native_location, native_path_from_location};
pub use windows::{
    decode_windows_path_units, encode_windows_path_units, windows_location_from_units,
};

#[derive(Debug, Error, Eq, PartialEq)]
pub enum NativePathError {
    #[error("location belongs to {stored}, but this process runs on {current}")]
    PlatformMismatch {
        stored: &'static str,
        current: &'static str,
    },
    #[error("Windows native path has an odd byte length")]
    InvalidWindowsEncoding,
}

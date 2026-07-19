use std::{
    ffi::{OsStr, OsString},
    path::{Path, PathBuf},
};

use shadow_domain::{AssetLocation, Platform};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum NativePathError {
    #[error("location belongs to {stored}, but this process runs on {current}")]
    PlatformMismatch {
        stored: &'static str,
        current: &'static str,
    },
    #[cfg(target_os = "windows")]
    #[error("Windows native path has an odd byte length")]
    InvalidWindowsEncoding,
}

pub(crate) fn encode_location(path: &Path) -> AssetLocation {
    AssetLocation::new(
        current_platform(),
        encode_os_str(path.as_os_str()),
        path.to_string_lossy(),
    )
}

pub(crate) fn decode_location(location: &AssetLocation) -> Result<PathBuf, NativePathError> {
    let current = current_platform();
    if location.platform != current {
        return Err(NativePathError::PlatformMismatch {
            stored: location.platform.as_str(),
            current: current.as_str(),
        });
    }

    #[cfg(unix)]
    let native_path = decode_os_string(&location.native_path);
    #[cfg(target_os = "windows")]
    let native_path = decode_os_string(&location.native_path)?;

    Ok(PathBuf::from(native_path))
}

#[cfg(target_os = "macos")]
const fn current_platform() -> Platform {
    Platform::MacOs
}

#[cfg(target_os = "windows")]
const fn current_platform() -> Platform {
    Platform::Windows
}

#[cfg(all(unix, not(target_os = "macos")))]
const fn current_platform() -> Platform {
    Platform::OtherUnix
}

#[cfg(unix)]
fn encode_os_str(value: &OsStr) -> Vec<u8> {
    use std::os::unix::ffi::OsStrExt;

    value.as_bytes().to_vec()
}

#[cfg(unix)]
fn decode_os_string(value: &[u8]) -> OsString {
    use std::os::unix::ffi::OsStringExt;

    OsString::from_vec(value.to_vec())
}

#[cfg(target_os = "windows")]
fn encode_os_str(value: &OsStr) -> Vec<u8> {
    use std::os::windows::ffi::OsStrExt;

    value
        .encode_wide()
        .flat_map(u16::to_le_bytes)
        .collect::<Vec<_>>()
}

#[cfg(target_os = "windows")]
fn decode_os_string(value: &[u8]) -> Result<OsString, NativePathError> {
    use std::os::windows::ffi::OsStringExt;

    let chunks = value.chunks_exact(2);
    if !chunks.remainder().is_empty() {
        return Err(NativePathError::InvalidWindowsEncoding);
    }
    let wide = chunks
        .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
        .collect::<Vec<_>>();
    Ok(OsString::from_wide(&wide))
}

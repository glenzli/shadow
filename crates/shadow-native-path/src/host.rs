use std::{
    ffi::{OsStr, OsString},
    path::{Path, PathBuf},
};

use shadow_domain::{AssetLocation, Platform};

use crate::NativePathError;
#[cfg(target_os = "windows")]
use crate::{decode_windows_path_units, encode_windows_path_units};

/// Encodes a path losslessly for the current platform.
#[must_use]
pub fn native_location(path: &Path) -> AssetLocation {
    AssetLocation::new(
        current_platform(),
        encode_os_str(path.as_os_str()),
        path.to_string_lossy(),
    )
}

/// Reconstructs a stored location for the current operating system.
///
/// `display_path` is deliberately ignored because it may be lossy or stale.
///
/// # Errors
///
/// Returns an error when the stored location belongs to another platform or a
/// Windows location contains an odd number of persisted bytes.
pub fn native_path_from_location(location: &AssetLocation) -> Result<PathBuf, NativePathError> {
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
pub const fn current_platform() -> Platform {
    Platform::MacOs
}

#[cfg(target_os = "windows")]
pub const fn current_platform() -> Platform {
    Platform::Windows
}

#[cfg(all(unix, not(target_os = "macos")))]
pub const fn current_platform() -> Platform {
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

    encode_windows_path_units(&value.encode_wide().collect::<Vec<_>>())
}

#[cfg(target_os = "windows")]
fn decode_os_string(value: &[u8]) -> Result<OsString, NativePathError> {
    use std::os::windows::ffi::OsStringExt;

    decode_windows_path_units(value).map(|units| OsString::from_wide(&units))
}

#[cfg(test)]
mod tests;

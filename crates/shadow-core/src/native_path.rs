use std::{ffi::OsStr, path::Path};

use shadow_domain::{AssetLocation, Platform};

pub(crate) fn encode_location(path: &Path) -> AssetLocation {
    AssetLocation::new(
        current_platform(),
        encode_os_str(path.as_os_str()),
        path.to_string_lossy(),
    )
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

#[cfg(target_os = "windows")]
fn encode_os_str(value: &OsStr) -> Vec<u8> {
    use std::os::windows::ffi::OsStrExt;

    value
        .encode_wide()
        .flat_map(u16::to_le_bytes)
        .collect::<Vec<_>>()
}

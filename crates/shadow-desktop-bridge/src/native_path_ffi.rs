//! Lossless native path projection at the Qt/CXX boundary.
//!
//! The CXX facade transports platform-native payloads and this owner performs
//! the one mapping into Shadow's durable [`AssetLocation`] contract. Display
//! text is never consulted while reconstructing a filesystem path.

use std::path::PathBuf;

use anyhow::{Result, bail};
use shadow_domain::{AssetLocation, Platform};
use shadow_native_path::{
    decode_windows_path_units, native_path_from_location, windows_location_from_units,
};

use crate::ffi;

pub(crate) fn path_from_ffi(input: &ffi::FfiNativePath) -> Result<PathBuf> {
    Ok(native_path_from_location(&location_from_ffi(input)?)?)
}

pub(crate) fn location_from_ffi(input: &ffi::FfiNativePath) -> Result<AssetLocation> {
    match input.platform {
        ffi::FfiNativePathPlatform::MacOs => {
            reject_windows_payload(input)?;
            Ok(AssetLocation::new(
                Platform::MacOs,
                input.unix_bytes.clone(),
                input.display_path.clone(),
            ))
        }
        ffi::FfiNativePathPlatform::Windows => {
            reject_unix_payload(input)?;
            Ok(windows_location_from_units(
                &input.windows_units,
                input.display_path.clone(),
            ))
        }
        ffi::FfiNativePathPlatform::OtherUnix => {
            reject_windows_payload(input)?;
            Ok(AssetLocation::new(
                Platform::OtherUnix,
                input.unix_bytes.clone(),
                input.display_path.clone(),
            ))
        }
        _ => bail!("native path uses an unknown platform discriminant"),
    }
}

pub(crate) fn location_to_ffi(location: &AssetLocation) -> Result<ffi::FfiNativePath> {
    let (platform, unix_bytes, windows_units) = match location.platform {
        Platform::MacOs => (
            ffi::FfiNativePathPlatform::MacOs,
            location.native_path.clone(),
            Vec::new(),
        ),
        Platform::Windows => (
            ffi::FfiNativePathPlatform::Windows,
            Vec::new(),
            decode_windows_path_units(&location.native_path)?,
        ),
        Platform::OtherUnix => (
            ffi::FfiNativePathPlatform::OtherUnix,
            location.native_path.clone(),
            Vec::new(),
        ),
    };
    Ok(ffi::FfiNativePath {
        platform,
        unix_bytes,
        windows_units,
        display_path: location.display_path.clone(),
    })
}

fn reject_windows_payload(input: &ffi::FfiNativePath) -> Result<()> {
    if input.windows_units.is_empty() {
        Ok(())
    } else {
        bail!("Unix native path must not carry Windows UTF-16 code units")
    }
}

fn reject_unix_payload(input: &ffi::FfiNativePath) -> Result<()> {
    if input.unix_bytes.is_empty() {
        Ok(())
    } else {
        bail!("Windows native path must not carry Unix path bytes")
    }
}

#[cfg(test)]
mod tests;

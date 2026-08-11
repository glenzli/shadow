use crate::NativePathError;
use shadow_domain::{AssetLocation, Platform};

/// Builds a durable Windows location directly from Qt or Win32 path code units.
///
/// The display string remains presentation-only; persisted identity uses the
/// exact little-endian code-unit bytes.
#[must_use]
pub fn windows_location_from_units(
    units: &[u16],
    display_path: impl Into<String>,
) -> AssetLocation {
    AssetLocation::new(
        Platform::Windows,
        encode_windows_path_units(units),
        display_path,
    )
}

/// Encodes Windows path code units in the persisted little-endian byte form.
///
/// This is deliberately a code-unit conversion, not a Unicode string
/// conversion, so unpaired surrogate values remain lossless.
#[must_use]
pub fn encode_windows_path_units(units: &[u16]) -> Vec<u8> {
    units.iter().flat_map(|unit| unit.to_le_bytes()).collect()
}

/// Decodes persisted Windows path bytes into their original UTF-16 code units.
///
/// # Errors
///
/// Returns [`NativePathError::InvalidWindowsEncoding`] when the byte length is
/// odd. Every complete `u16` value is retained, including unpaired surrogates.
pub fn decode_windows_path_units(bytes: &[u8]) -> Result<Vec<u16>, NativePathError> {
    let chunks = bytes.chunks_exact(2);
    if !chunks.remainder().is_empty() {
        return Err(NativePathError::InvalidWindowsEncoding);
    }
    Ok(chunks
        .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
        .collect())
}

#[cfg(test)]
mod tests;

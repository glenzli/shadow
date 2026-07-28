//! Structural input and canonical JSON validation shared by export owners.

use shadow_domain::AssetLocation;

use crate::CatalogError;

use super::model::{ExportFailure, NewExportOutputReceipt};

pub(super) fn validate_preset_name(value: &str) -> Result<(), CatalogError> {
    let trimmed = value.trim();
    if trimmed.is_empty() || trimmed.len() > 256 || trimmed.contains('\0') {
        return Err(CatalogError::InvalidExport(
            "export preset names must contain 1 through 256 non-NUL bytes".into(),
        ));
    }
    Ok(())
}

pub(super) fn validate_output_location(location: &AssetLocation) -> Result<(), CatalogError> {
    if location.native_path.is_empty()
        || location.display_path.trim().is_empty()
        || location.display_path.len() > 16 * 1024
        || location.display_path.contains('\0')
    {
        return Err(CatalogError::InvalidExport(
            "export output locations require nonempty native and display paths".into(),
        ));
    }
    Ok(())
}

pub(super) fn validate_receipt(
    receipt: &NewExportOutputReceipt,
    expected_output: &AssetLocation,
) -> Result<(), CatalogError> {
    validate_output_location(&receipt.output)?;
    if &receipt.output != expected_output {
        return Err(CatalogError::InvalidExport(
            "an output receipt must name the immutable item output target".into(),
        ));
    }
    if !valid_short_token(&receipt.output_format, 64) {
        return Err(CatalogError::InvalidExport(
            "export output format must be printable text up to 64 bytes".into(),
        ));
    }
    let _ = normalized_json_object(&receipt.receipt_json, "output receipt")?;
    Ok(())
}

pub(super) fn validate_failure(failure: &ExportFailure) -> Result<(), CatalogError> {
    if !valid_short_token(&failure.code, 128)
        || failure.message.trim().is_empty()
        || failure.message.len() > 8192
        || failure.message.contains('\0')
    {
        return Err(CatalogError::InvalidExport(
            "export failure code/message are outside their supported bounds".into(),
        ));
    }
    Ok(())
}

fn valid_short_token(value: &str, maximum: usize) -> bool {
    !value.is_empty()
        && value.len() <= maximum
        && value
            .bytes()
            .all(|byte| byte.is_ascii_graphic() || byte == b' ')
}

pub(super) fn normalized_json_object(value: &str, field: &str) -> Result<String, CatalogError> {
    let parsed: serde_json::Value = serde_json::from_str(value).map_err(|error| {
        CatalogError::InvalidExport(format!("{field} must be valid JSON: {error}"))
    })?;
    if !parsed.is_object() {
        return Err(CatalogError::InvalidExport(format!(
            "{field} must be a JSON object"
        )));
    }
    serde_json::to_string(&parsed).map_err(|error| {
        CatalogError::InvalidExport(format!("{field} cannot be normalized: {error}"))
    })
}

pub(super) fn validate_stored_json_snapshot(
    value: &str,
    expected_digest: [u8; 32],
    field: &str,
) -> Result<(), CatalogError> {
    let normalized = normalized_json_object(value, field)?;
    if normalized != value {
        return Err(CatalogError::InvalidExport(format!(
            "persisted {field} JSON is not canonical"
        )));
    }
    if json_digest(value) != expected_digest {
        return Err(CatalogError::InvalidExport(format!(
            "persisted {field} digest does not match its JSON"
        )));
    }
    Ok(())
}

pub(super) fn json_digest(value: &str) -> [u8; 32] {
    *blake3::hash(value.as_bytes()).as_bytes()
}

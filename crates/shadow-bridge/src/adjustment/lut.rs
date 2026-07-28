//! Immutable 3D LUT document bounds and blend-intensity validation.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// Hard bound for one immutable `.cube` document crossing the render bridge.
pub const MAX_LUT_DOCUMENT_BYTES: usize = 16 * 1_024 * 1_024;

pub(super) fn validate_lut(document: &[u8], intensity: f64) -> Result<(), BridgeError> {
    validate_finite_render_parameter(intensity)?;
    if !(0.0..=1.0).contains(&intensity) {
        return Err(BridgeError::InvalidEditRequest(
            "3D LUT intensity must be in 0..=1",
        ));
    }
    if document.is_empty() {
        if intensity == 0.0 {
            Ok(())
        } else {
            Err(BridgeError::InvalidEditRequest(
                "an active 3D LUT requires a document",
            ))
        }
    } else if document.len() <= MAX_LUT_DOCUMENT_BYTES {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "3D LUT document exceeds 16 MiB",
        ))
    }
}

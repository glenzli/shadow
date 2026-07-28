use crate::error::BridgeError;

pub(super) fn validate_finite_render_parameter(value: f64) -> Result<(), BridgeError> {
    if value.is_finite() {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "adjustment render parameters must be finite",
        ))
    }
}

pub(super) fn validate_inclusive(
    value: f64,
    minimum: f64,
    maximum: f64,
    message: &'static str,
) -> Result<(), BridgeError> {
    if value.is_finite() && (minimum..=maximum).contains(&value) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(message))
    }
}

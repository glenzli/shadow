//! Guided Selective Tone schema and normalized authored-parameter validation.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// Numeric parameter contract for the complete guided scene-linear Selective Tone filter.
pub const SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION: u32 = 1;
/// Numeric executor revision for the complete guided Selective Tone filter.
pub const SELECTIVE_TONE_IMPLEMENTATION_VERSION: u32 = 1;

/// Guided local tonal zones in the processed linear-light RGB working space. Values are normalized
/// user intent in `[-1, 1]`; the executor owns the versioned EV weighting, mask, and strength.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SelectiveToneParameters {
    pub highlights: f64,
    pub shadows: f64,
    pub whites: f64,
    pub blacks: f64,
}

impl Default for SelectiveToneParameters {
    fn default() -> Self {
        Self {
            highlights: 0.0,
            shadows: 0.0,
            whites: 0.0,
            blacks: 0.0,
        }
    }
}

pub(super) fn validate_selective_tone(
    parameters: SelectiveToneParameters,
) -> Result<(), BridgeError> {
    for value in [
        parameters.highlights,
        parameters.shadows,
        parameters.whites,
        parameters.blacks,
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "selective tone values must be in -1..=1",
            ));
        }
    }
    Ok(())
}

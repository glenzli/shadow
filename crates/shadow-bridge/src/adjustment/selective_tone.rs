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
    /// Relative channel suppression in source-evidenced RAW highlights. Values are in `[0, 1]`;
    /// their common component is ignored so equal R/G/B values remain an exact no-op.
    pub highlight_red_suppression: f64,
    pub highlight_green_suppression: f64,
    pub highlight_blue_suppression: f64,
}

impl Default for SelectiveToneParameters {
    fn default() -> Self {
        Self {
            highlights: 0.0,
            shadows: 0.0,
            whites: 0.0,
            blacks: 0.0,
            highlight_red_suppression: 0.0,
            highlight_green_suppression: 0.0,
            highlight_blue_suppression: 0.0,
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
    for value in [
        parameters.highlight_red_suppression,
        parameters.highlight_green_suppression,
        parameters.highlight_blue_suppression,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "highlight channel suppression values must be in 0..=1",
            ));
        }
    }
    Ok(())
}

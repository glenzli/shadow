//! Oklab Color Warper lattice contract and fail-closed parameter validation.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// Fixed Color Warper mesh dimensions. The typed bridge deliberately mirrors
/// the native 5×5 Oklab a/b lattice rather than exposing a second UI-specific
/// mesh shape.
pub const OKLAB_COLOR_WARPER_GRID_SIDE: usize = 5;
pub const OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT: usize =
    OKLAB_COLOR_WARPER_GRID_SIDE * OKLAB_COLOR_WARPER_GRID_SIDE;
pub const OKLAB_COLOR_WARPER_MAXIMUM_OFFSET: f64 = 0.32;
pub const OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION: u32 = 1;
/// One row-major target displacement in the immutable Oklab Color Warper
/// lattice. The source lattice positions are implicit and stable, which keeps
/// Recipes compact and makes a later mesh editor deterministic.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct OklabColorWarperControlPoint {
    pub a_offset: f64,
    pub b_offset: f64,
}

/// A fixed 5×5 Oklab a/b displacement lattice. Unlike the hue-keyed Color
/// Mixer and sampled Point Color ranges, this is one connected chroma field
/// that can be attached to a single masked Grade Node.
#[derive(Debug, Clone, PartialEq)]
pub struct OklabColorWarperParameters {
    pub control_points: [OklabColorWarperControlPoint; OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT],
    pub strength: f64,
}

impl Default for OklabColorWarperParameters {
    fn default() -> Self {
        Self {
            control_points: [OklabColorWarperControlPoint {
                a_offset: 0.0,
                b_offset: 0.0,
            }; OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT],
            strength: 1.0,
        }
    }
}

pub(super) fn validate_oklab_color_warper(
    parameters: &OklabColorWarperParameters,
) -> Result<(), BridgeError> {
    validate_finite_render_parameter(parameters.strength)?;
    if !(0.0..=1.0).contains(&parameters.strength) {
        return Err(BridgeError::InvalidEditRequest(
            "Oklab Color Warper strength must be normalized to 0..=1",
        ));
    }
    for point in &parameters.control_points {
        for value in [point.a_offset, point.b_offset] {
            validate_finite_render_parameter(value)?;
            if value.abs() > OKLAB_COLOR_WARPER_MAXIMUM_OFFSET {
                return Err(BridgeError::InvalidEditRequest(
                    "Oklab Color Warper control offsets exceed the declared mesh extent",
                ));
            }
        }
    }
    Ok(())
}

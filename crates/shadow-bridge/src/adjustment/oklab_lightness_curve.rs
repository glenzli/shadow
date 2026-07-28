//! Oklab-lightness curve schema and fail-closed geometry validation.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// Numeric contract for Shadow's sole Oklab-L perceptual curve.
pub const OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION: u32 = 1;
/// Mirrors the CPU reference Tone Curve bound without exposing a C++ type.
pub const MAX_TONE_CURVE_POINTS: usize = 256;

/// One authored point in Shadow's perceptual tone-curve contract.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ToneCurvePoint {
    pub x: f64,
    pub y: f64,
}

/// A smooth curve for the Oklab L axis alone. The a/b opponent axes are retained exactly, so its
/// normal use is tonal shaping without a hue or chroma adjustment.
#[derive(Debug, Clone, PartialEq)]
pub struct OklabLightnessToneCurve {
    pub lightness: Vec<ToneCurvePoint>,
}

impl Default for OklabLightnessToneCurve {
    fn default() -> Self {
        Self {
            lightness: vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
        }
    }
}

#[allow(clippy::float_cmp)] // Tone Curve schemas require exact normalized endpoints.
pub(super) fn validate_tone_curve_points(points: &[ToneCurvePoint]) -> Result<(), BridgeError> {
    if !(2..=MAX_TONE_CURVE_POINTS).contains(&points.len()) {
        return Err(BridgeError::InvalidEditRequest(
            "tone curve must contain 2 through 256 points",
        ));
    }
    if points.first().is_none_or(|point| point.x != 0.0)
        || points.last().is_none_or(|point| point.x != 1.0)
    {
        return Err(BridgeError::InvalidEditRequest(
            "tone curve x coordinates must start at zero and end at one",
        ));
    }
    let mut previous: Option<ToneCurvePoint> = None;
    for point in points {
        validate_finite_render_parameter(point.x)?;
        validate_finite_render_parameter(point.y)?;
        if let Some(previous_point) = previous {
            if point.x <= previous_point.x {
                return Err(BridgeError::InvalidEditRequest(
                    "tone curve x coordinates must be strictly increasing",
                ));
            }
            let slope = (point.y - previous_point.y) / (point.x - previous_point.x);
            if !slope.is_finite() {
                return Err(BridgeError::InvalidEditRequest(
                    "tone curve segment slopes must be finite",
                ));
            }
        }
        previous = Some(*point);
    }
    Ok(())
}

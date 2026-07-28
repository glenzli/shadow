//! Normalized local-mask shapes and their spatial invariants.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// A normalized spatial mask ready for the native layer mixer.
#[derive(Debug, Clone, PartialEq)]
pub enum AdjustmentLocalMask {
    LinearGradient {
        start_x: f64,
        start_y: f64,
        end_x: f64,
        end_y: f64,
        invert: bool,
    },
    RadialGradient {
        center_x: f64,
        center_y: f64,
        radius_x: f64,
        radius_y: f64,
        feather: f64,
        invert: bool,
    },
    Brush {
        points: Vec<AdjustmentMaskBrushPoint>,
        radius: f64,
        feather: f64,
        invert: bool,
    },
}

/// One normalized freehand-mask sample prepared for the native mixer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentMaskBrushPoint {
    pub x: f64,
    pub y: f64,
    pub begins_stroke: bool,
}

pub(super) fn validate_adjustment_local_mask(
    mask: &AdjustmentLocalMask,
) -> Result<(), BridgeError> {
    let unit = |value: f64| {
        validate_finite_render_parameter(value)?;
        if (0.0..=1.0).contains(&value) {
            Ok(())
        } else {
            Err(BridgeError::InvalidEditRequest(
                "local-mask coordinates must be normalized to 0..=1",
            ))
        }
    };
    match mask {
        AdjustmentLocalMask::LinearGradient {
            start_x,
            start_y,
            end_x,
            end_y,
            ..
        } => {
            for value in [*start_x, *start_y, *end_x, *end_y] {
                unit(value)?;
            }
            let dx = *end_x - *start_x;
            let dy = *end_y - *start_y;
            if dx.mul_add(dx, dy * dy) <= f64::EPSILON {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask linear gradient must have a non-zero direction",
                ));
            }
        }
        AdjustmentLocalMask::RadialGradient {
            center_x,
            center_y,
            radius_x,
            radius_y,
            feather,
            ..
        } => {
            for value in [*center_x, *center_y, *radius_x, *radius_y, *feather] {
                unit(value)?;
            }
            if *radius_x <= 0.0 || *radius_y <= 0.0 {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask radial gradient radii must both be greater than zero",
                ));
            }
        }
        AdjustmentLocalMask::Brush {
            points,
            radius,
            feather,
            ..
        } => {
            for value in [*radius, *feather] {
                unit(value)?;
            }
            if *radius <= 0.0 {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask brush radius must be greater than zero",
                ));
            }
            if points.len() > 4_096 {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask brush supports at most 4096 points",
                ));
            }
            for point in points {
                unit(point.x)?;
                unit(point.y)?;
            }
        }
    }
    Ok(())
}

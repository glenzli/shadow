//! Photo-private Liquify values at the executable render-plan boundary.
//!
//! These values preserve normalized authored paths. Resolution-specific stamp
//! preparation remains native execution detail, so preview, detail, and export
//! can rebuild one equivalent displacement plan for their own raster size.

use super::parameter_validation::{validate_finite_render_parameter, validate_inclusive};
use crate::BridgeError;

/// Maximum authored gestures carried by one optional Liquify structural node.
pub const MAX_ADJUSTMENT_LIQUIFY_STROKES: usize = 128;
/// Maximum pointer samples carried by one authored Liquify gesture.
pub const MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE: usize = 2_048;

/// One pressure-bearing sample in normalized, uncropped image-edge coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentLiquifyPoint {
    pub x: f64,
    pub y: f64,
    pub pressure: f64,
}

/// One authored push-brush gesture.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentLiquifyPushStroke {
    pub points: Vec<AdjustmentLiquifyPoint>,
    /// Brush radius as a fraction of the original image's shorter edge.
    pub radius: f64,
    /// Normalized displacement gain.
    pub strength: f64,
    /// Normalized inner falloff; `1` keeps a harder core.
    pub hardness: f64,
}

/// The optional, singleton Liquify payload in a photo render plan.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentLiquify {
    pub strokes: Vec<AdjustmentLiquifyPushStroke>,
}

impl AdjustmentLiquify {
    pub(super) fn validate(&self) -> Result<(), BridgeError> {
        if self.strokes.is_empty() || self.strokes.len() > MAX_ADJUSTMENT_LIQUIFY_STROKES {
            return Err(BridgeError::InvalidEditRequest(
                "photo liquify must contain 1 through 128 gestures",
            ));
        }
        for stroke in &self.strokes {
            validate_push_stroke(stroke)?;
        }
        Ok(())
    }
}

fn validate_push_stroke(stroke: &AdjustmentLiquifyPushStroke) -> Result<(), BridgeError> {
    if stroke.points.len() < 2 || stroke.points.len() > MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify push gesture must contain 2 through 2048 samples",
        ));
    }
    validate_unit_parameter(stroke.radius)?;
    validate_unit_parameter(stroke.strength)?;
    validate_unit_parameter(stroke.hardness)?;
    if stroke.radius == 0.0 {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify push radius must be greater than zero",
        ));
    }
    if stroke.strength == 0.0 {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify push strength must be greater than zero",
        ));
    }
    for point in &stroke.points {
        validate_unit_parameter(point.x)?;
        validate_unit_parameter(point.y)?;
        validate_unit_parameter(point.pressure)?;
    }
    if !stroke.points.windows(2).any(|segment| {
        let from = segment[0];
        let to = segment[1];
        let displacement_x = from.x - to.x;
        let displacement_y = from.y - to.y;
        displacement_x.mul_add(displacement_x, displacement_y * displacement_y) > 0.0
            && (from.pressure > 0.0 || to.pressure > 0.0)
    }) {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify push gesture must contain effective pressured movement",
        ));
    }
    Ok(())
}

fn validate_unit_parameter(value: f64) -> Result<(), BridgeError> {
    validate_finite_render_parameter(value)?;
    validate_inclusive(
        value,
        0.0,
        1.0,
        "photo liquify parameters must be in [0, 1]",
    )
}

#[cfg(test)]
mod tests;

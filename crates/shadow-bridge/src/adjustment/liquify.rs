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

/// One authored local restoration gesture.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentLiquifyReconstructStroke {
    pub points: Vec<AdjustmentLiquifyPoint>,
    /// Brush radius as a fraction of the original image's shorter edge.
    pub radius: f64,
    /// Normalized amount blended toward the identity mapping.
    pub strength: f64,
    /// Normalized inner falloff; `1` keeps a harder core.
    pub hardness: f64,
}

/// One ordered authored operation in the singleton Liquify node.
#[derive(Debug, Clone, PartialEq)]
pub enum AdjustmentLiquifyStroke {
    Push(AdjustmentLiquifyPushStroke),
    Reconstruct(AdjustmentLiquifyReconstructStroke),
}

/// The optional, singleton Liquify payload in a photo render plan.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentLiquify {
    /// Whether the materialized node contributes deformation. A bypassed
    /// payload remains present so detail-session capability admission does not
    /// churn while the user compares before and after.
    pub enabled: bool,
    pub strokes: Vec<AdjustmentLiquifyStroke>,
}

impl AdjustmentLiquify {
    pub(super) fn validate(&self) -> Result<(), BridgeError> {
        if self.strokes.is_empty() || self.strokes.len() > MAX_ADJUSTMENT_LIQUIFY_STROKES {
            return Err(BridgeError::InvalidEditRequest(
                "photo liquify must contain 1 through 128 gestures",
            ));
        }
        let mut has_prior_deformation = false;
        for stroke in &self.strokes {
            match stroke {
                AdjustmentLiquifyStroke::Push(stroke) => {
                    validate_push_stroke(stroke)?;
                    has_prior_deformation = true;
                }
                AdjustmentLiquifyStroke::Reconstruct(stroke) => {
                    if !has_prior_deformation {
                        return Err(BridgeError::InvalidEditRequest(
                            "photo liquify reconstruct gesture requires earlier deformation",
                        ));
                    }
                    validate_reconstruct_stroke(stroke)?;
                }
            }
        }
        Ok(())
    }
}

fn validate_reconstruct_stroke(
    stroke: &AdjustmentLiquifyReconstructStroke,
) -> Result<(), BridgeError> {
    if stroke.points.is_empty() || stroke.points.len() > MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify reconstruct gesture must contain 1 through 2048 samples",
        ));
    }
    validate_brush(stroke.radius, stroke.strength, stroke.hardness)?;
    validate_points(&stroke.points)?;
    if !stroke.points.iter().any(|point| point.pressure > 0.0) {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify reconstruct gesture must contain effective pressure",
        ));
    }
    Ok(())
}

fn validate_push_stroke(stroke: &AdjustmentLiquifyPushStroke) -> Result<(), BridgeError> {
    if stroke.points.len() < 2 || stroke.points.len() > MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify push gesture must contain 2 through 2048 samples",
        ));
    }
    validate_brush(stroke.radius, stroke.strength, stroke.hardness)?;
    validate_points(&stroke.points)?;
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

fn validate_brush(radius: f64, strength: f64, hardness: f64) -> Result<(), BridgeError> {
    validate_unit_parameter(radius)?;
    validate_unit_parameter(strength)?;
    validate_unit_parameter(hardness)?;
    if radius == 0.0 {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify brush radius must be greater than zero",
        ));
    }
    if strength == 0.0 {
        return Err(BridgeError::InvalidEditRequest(
            "photo liquify brush strength must be greater than zero",
        ));
    }
    Ok(())
}

fn validate_points(points: &[AdjustmentLiquifyPoint]) -> Result<(), BridgeError> {
    for point in points {
        validate_unit_parameter(point.x)?;
        validate_unit_parameter(point.y)?;
        validate_unit_parameter(point.pressure)?;
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

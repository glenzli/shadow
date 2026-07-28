//! Bounded spot-heal targets and continuous swept repair/clone strokes.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// One bounded source-space target for a spot-heal operation.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentSpotHealTarget {
    pub center_x: f64,
    pub center_y: f64,
    pub radius_level_zero_pixels: u16,
    /// 0 = heal, 1 = clone.
    pub mode: u8,
    pub source_offset_x_radii: f64,
    pub source_offset_y_radii: f64,
    pub feather: f64,
}

/// One normalized sampled point in a continuous repair/clone stroke.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentRetouchStrokePoint {
    pub x: f64,
    pub y: f64,
}

/// A bounded swept brush region for a spot-heal operation.
///
/// The source offset is measured in brush radii and stays fixed over the whole stroke, so clone
/// source and target keep the same shape.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRetouchStroke {
    pub points: Vec<AdjustmentRetouchStrokePoint>,
    pub radius_level_zero_pixels: u16,
    /// 0 = heal, 1 = clone.
    pub mode: u8,
    pub source_offset_x_radii: f64,
    pub source_offset_y_radii: f64,
    pub feather: f64,
}

pub(super) fn validate_spot_heal(
    targets: &[AdjustmentSpotHealTarget],
    strokes: &[AdjustmentRetouchStroke],
) -> Result<(), BridgeError> {
    if (targets.is_empty() && strokes.is_empty()) || targets.len() > 64 {
        return Err(BridgeError::InvalidEditRequest(
            "spot-heal must contain a repair target or continuous stroke",
        ));
    }
    for target in targets {
        for value in [
            target.center_x,
            target.center_y,
            target.source_offset_x_radii,
            target.source_offset_y_radii,
            target.feather,
        ] {
            validate_finite_render_parameter(value)?;
        }
        for value in [target.center_x, target.center_y, target.feather] {
            if !(0.0..=1.0).contains(&value) {
                return Err(BridgeError::InvalidEditRequest(
                    "spot-heal coordinates and feather must be normalized to 0..=1",
                ));
            }
        }
        if target.mode > 1
            || !(-2.0..=2.0).contains(&target.source_offset_x_radii)
            || !(-2.0..=2.0).contains(&target.source_offset_y_radii)
            || !(1..=128).contains(&target.radius_level_zero_pixels)
        {
            return Err(BridgeError::InvalidEditRequest(
                "spot-heal mode, source offset, or radius is outside its supported range",
            ));
        }
    }
    if strokes.len() > 64 {
        return Err(BridgeError::InvalidEditRequest(
            "spot-heal supports at most 64 continuous strokes",
        ));
    }
    for stroke in strokes {
        if !(1..=512).contains(&stroke.points.len()) {
            return Err(BridgeError::InvalidEditRequest(
                "a continuous repair stroke must contain 1 through 512 points",
            ));
        }
        for value in [
            stroke.source_offset_x_radii,
            stroke.source_offset_y_radii,
            stroke.feather,
        ] {
            validate_finite_render_parameter(value)?;
        }
        if stroke.mode > 1
            || !(-2.0..=2.0).contains(&stroke.source_offset_x_radii)
            || !(-2.0..=2.0).contains(&stroke.source_offset_y_radii)
            || !(0.0..=1.0).contains(&stroke.feather)
            || !(1..=128).contains(&stroke.radius_level_zero_pixels)
        {
            return Err(BridgeError::InvalidEditRequest(
                "continuous spot-heal behavior is outside the supported range",
            ));
        }
        for point in &stroke.points {
            validate_finite_render_parameter(point.x)?;
            validate_finite_render_parameter(point.y)?;
            if !(0.0..=1.0).contains(&point.x) || !(0.0..=1.0).contains(&point.y) {
                return Err(BridgeError::InvalidEditRequest(
                    "continuous spot-heal points must be normalized to 0..=1",
                ));
            }
        }
    }
    Ok(())
}

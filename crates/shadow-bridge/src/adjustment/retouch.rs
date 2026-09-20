//! Image-bounded spot-heal targets and continuous swept repair/clone strokes.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

const MAX_RETOUCH_DETAIL_APRON_LEVEL_ZERO_PIXELS: f64 = 512.0;

fn source_offset_fits_detail_apron(
    radius: u16,
    horizontal: f64,
    vertical: f64,
    source_scale: f64,
) -> bool {
    let maximum =
        (MAX_RETOUCH_DETAIL_APRON_LEVEL_ZERO_PIXELS - 1.0) / f64::from(radius) - source_scale;
    horizontal.abs() <= maximum && vertical.abs() <= maximum
}

fn source_transform_is_valid(rotation_degrees: f64, scale: f64) -> bool {
    (-180.0..=180.0).contains(&rotation_degrees) && (0.25..=4.0).contains(&scale)
}

fn validate_source_transform_and_apron(
    radius_level_zero_pixels: u16,
    horizontal_offset_radii: f64,
    vertical_offset_radii: f64,
    rotation_degrees: f64,
    source_scale: f64,
    subject: &'static str,
) -> Result<(), BridgeError> {
    if !source_transform_is_valid(rotation_degrees, source_scale) {
        return Err(BridgeError::InvalidEditRequest(match subject {
            "spot-heal" => "spot-heal source transform is outside its supported range",
            _ => "continuous spot-heal source transform is outside its supported range",
        }));
    }
    if !source_offset_fits_detail_apron(
        radius_level_zero_pixels,
        horizontal_offset_radii,
        vertical_offset_radii,
        source_scale,
    ) {
        return Err(BridgeError::InvalidEditRequest(match subject {
            "spot-heal" => "spot-heal donor source exceeds the full-detail apron",
            _ => "continuous spot-heal donor source exceeds the full-detail apron",
        }));
    }
    Ok(())
}

/// One bounded source-space target for a spot-heal operation.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentSpotHealTarget {
    pub center_x: f64,
    pub center_y: f64,
    pub radius_level_zero_pixels: u16,
    /// 0 = heal, 1 = clone, 2 = structure-preserving heal, 3 = tone, 4 = texture.
    pub mode: u8,
    /// Finite horizontal donor displacement in brush radii.
    pub source_offset_x_radii: f64,
    /// Finite vertical donor displacement in brush radii.
    pub source_offset_y_radii: f64,
    pub source_rotation_degrees: f64,
    pub source_scale: f64,
    pub source_flip_horizontal: bool,
    pub source_flip_vertical: bool,
    pub feather: f64,
    pub strength: f64,
    pub frequency_radius: u16,
}

/// One normalized sampled point in a continuous repair/clone stroke.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentRetouchStrokePoint {
    pub x: f64,
    pub y: f64,
}

/// A bounded swept brush region for a spot-heal operation.
///
/// The source offset is measured in brush radii and stays fixed over the whole
/// stroke, so Heal and Clone donors keep the target shape.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRetouchStroke {
    pub points: Vec<AdjustmentRetouchStrokePoint>,
    pub radius_level_zero_pixels: u16,
    /// 0 = heal, 1 = clone, 2 = structure-preserving heal, 3 = tone, 4 = texture.
    pub mode: u8,
    /// Finite horizontal donor displacement in brush radii.
    pub source_offset_x_radii: f64,
    /// Finite vertical donor displacement in brush radii.
    pub source_offset_y_radii: f64,
    pub source_rotation_degrees: f64,
    pub source_scale: f64,
    pub source_flip_horizontal: bool,
    pub source_flip_vertical: bool,
    pub feather: f64,
    pub strength: f64,
    pub frequency_radius: u16,
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
            target.source_rotation_degrees,
            target.source_scale,
            target.feather,
            target.strength,
        ] {
            validate_finite_render_parameter(value)?;
        }
        for value in [
            target.center_x,
            target.center_y,
            target.feather,
            target.strength,
        ] {
            if !(0.0..=1.0).contains(&value) {
                return Err(BridgeError::InvalidEditRequest(
                    "spot-heal coordinates, feather, and strength must be normalized to 0..=1",
                ));
            }
        }
        if target.mode > 4
            || !(2..=32).contains(&target.frequency_radius)
            || !(1..=128).contains(&target.radius_level_zero_pixels)
        {
            return Err(BridgeError::InvalidEditRequest(
                "spot-heal mode or radius is outside its supported range",
            ));
        }
        validate_source_transform_and_apron(
            target.radius_level_zero_pixels,
            target.source_offset_x_radii,
            target.source_offset_y_radii,
            target.source_rotation_degrees,
            target.source_scale,
            "spot-heal",
        )?;
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
            stroke.source_rotation_degrees,
            stroke.source_scale,
            stroke.feather,
            stroke.strength,
        ] {
            validate_finite_render_parameter(value)?;
        }
        if stroke.mode > 4
            || !(2..=32).contains(&stroke.frequency_radius)
            || !(0.0..=1.0).contains(&stroke.feather)
            || !(0.0..=1.0).contains(&stroke.strength)
            || !(1..=128).contains(&stroke.radius_level_zero_pixels)
        {
            return Err(BridgeError::InvalidEditRequest(
                "continuous spot-heal behavior is outside the supported range",
            ));
        }
        validate_source_transform_and_apron(
            stroke.radius_level_zero_pixels,
            stroke.source_offset_x_radii,
            stroke.source_offset_y_radii,
            stroke.source_rotation_degrees,
            stroke.source_scale,
            "continuous spot-heal",
        )?;
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

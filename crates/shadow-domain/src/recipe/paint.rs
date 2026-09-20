//! Photo-local, replayable paint layers. Coordinates are original-image normalized;
//! radius is a fraction of the original shorter side, independent of preview scale.

use super::{RecipeValidationError, UnitInterval};
use crate::LayerInstanceId;
use serde::{Deserialize, Serialize};

pub const MAX_PAINT_LAYERS: usize = 8;
pub const MAX_PAINT_STROKES: usize = 128;
pub const MAX_PAINT_POINTS: usize = 2048;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PaintBlendMode {
    Normal,
    Color,
    SoftLight,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct PaintPoint {
    pub x: UnitInterval,
    pub y: UnitInterval,
    pub pressure: UnitInterval,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PaintStroke {
    pub points: Vec<PaintPoint>,
    pub radius: UnitInterval,
    pub hardness: UnitInterval,
    pub opacity: UnitInterval,
    pub flow: UnitInterval,
    /// Unassociated display-sRGB, converted to the working space by the renderer.
    pub color: [UnitInterval; 3],
    pub erase: bool,
    #[serde(default = "one", skip_serializing_if = "is_one")]
    pub roundness: f64,
    #[serde(default, skip_serializing_if = "is_zero")]
    pub angle_degrees: f64,
    /// Distance between dabs as a fraction of the full brush diameter.
    #[serde(default = "legacy_spacing", skip_serializing_if = "is_legacy_spacing")]
    pub spacing: f64,
    /// 0 solid, 1 fine grain, 2 soft speckle; deterministic procedural grayscale tips.
    #[serde(default, skip_serializing_if = "is_zero_texture")]
    pub texture: u8,
    #[serde(default = "half", skip_serializing_if = "is_half")]
    pub texture_strength: f64,
    #[serde(default, skip_serializing_if = "is_false")]
    pub pressure_size: bool,
    #[serde(default = "yes", skip_serializing_if = "is_true")]
    pub pressure_flow: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PaintLayer {
    pub id: LayerInstanceId,
    pub label: String,
    pub enabled: bool,
    pub opacity: UnitInterval,
    pub blend: PaintBlendMode,
    pub coordinate_width: u32,
    pub coordinate_height: u32,
    pub strokes: Vec<PaintStroke>,
}

fn one() -> f64 {
    1.0
}
fn half() -> f64 {
    0.5
}
fn legacy_spacing() -> f64 {
    0.125
}
fn yes() -> bool {
    true
}
fn is_one(v: &f64) -> bool {
    *v == 1.0
}
fn is_half(v: &f64) -> bool {
    *v == 0.5
}
fn is_zero(v: &f64) -> bool {
    *v == 0.0
}
fn is_legacy_spacing(v: &f64) -> bool {
    *v == 0.125
}
fn is_zero_texture(v: &u8) -> bool {
    *v == 0
}
fn is_false(v: &bool) -> bool {
    !*v
}
fn is_true(v: &bool) -> bool {
    *v
}

impl PaintLayer {
    /// Validates the authored coordinate space and bounded stroke workload.
    ///
    /// # Errors
    /// Returns `InvalidPaint` for invalid labels, dimensions, radii or exceeded budgets.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        let invalid = |reason| RecipeValidationError::InvalidPaint(reason);
        if self.label.trim().is_empty() || self.label.len() > 256 {
            return Err(invalid("layer label must contain 1 through 256 bytes"));
        }
        if self.coordinate_width == 0
            || self.coordinate_height == 0
            || self.coordinate_width > 131_072
            || self.coordinate_height > 131_072
        {
            return Err(invalid("original coordinate dimensions are invalid"));
        }
        if self.strokes.len() > MAX_PAINT_STROKES {
            return Err(invalid("a layer supports at most 128 strokes"));
        }
        let mut total = 0usize;
        let mut dabs = 0.0;
        let short = f64::from(self.coordinate_width.min(self.coordinate_height));
        for stroke in &self.strokes {
            total += stroke.points.len();
            if stroke.points.is_empty() || stroke.points.len() > MAX_PAINT_POINTS || total > 32_768
            {
                return Err(invalid("paint point budget exceeded or empty stroke"));
            }
            if !(0.0001..=0.25).contains(&stroke.radius.get()) {
                return Err(invalid("paint radius must be within 0.0001 through 0.25"));
            }
            if !stroke.roundness.is_finite()
                || !(0.1..=1.0).contains(&stroke.roundness)
                || !stroke.angle_degrees.is_finite()
                || !(-180.0..=180.0).contains(&stroke.angle_degrees)
                || !stroke.spacing.is_finite()
                || !(0.02..=1.0).contains(&stroke.spacing)
                || stroke.texture > 2
                || !stroke.texture_strength.is_finite()
                || !(0.0..=1.0).contains(&stroke.texture_strength)
            {
                return Err(invalid("invalid brush tip or dynamics"));
            }
            let distance: f64 = stroke
                .points
                .windows(2)
                .map(|p| {
                    ((p[1].x.get() - p[0].x.get()) * f64::from(self.coordinate_width) / short)
                        .hypot(
                            (p[1].y.get() - p[0].y.get()) * f64::from(self.coordinate_height)
                                / short,
                        )
                })
                .sum();
            let count = 1.0
                + distance
                    / (stroke.radius.get()
                        * 2.0
                        * stroke.spacing
                        * if stroke.pressure_size { 0.1 } else { 1.0 });
            dabs += count;
            if count > 32_760.0 || dabs > 262_144.0 {
                return Err(invalid("paint dab budget exceeded"));
            }
        }
        Ok(())
    }
}

pub(super) fn validate_paint_layers(layers: &[PaintLayer]) -> Result<(), RecipeValidationError> {
    if layers.len() > MAX_PAINT_LAYERS {
        return Err(RecipeValidationError::InvalidPaint(
            "at most eight paint layers are supported",
        ));
    }
    let mut ids = std::collections::HashSet::new();
    for layer in layers {
        layer.validate()?;
        if !ids.insert(layer.id) {
            return Err(RecipeValidationError::InvalidPaint(
                "duplicate paint layer identity",
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;

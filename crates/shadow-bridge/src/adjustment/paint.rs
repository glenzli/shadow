//! Renderer-neutral bounded photographic paint wire.
use super::BridgeError;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentPaintPoint {
    pub x: f64,
    pub y: f64,
    pub pressure: f64,
}
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentPaintStroke {
    pub points: Vec<AdjustmentPaintPoint>,
    pub radius: f64,
    pub hardness: f64,
    pub opacity: f64,
    pub flow: f64,
    pub color: [f64; 3],
    pub erase: bool,
    pub roundness: f64,
    pub angle_degrees: f64,
    pub spacing: f64,
    pub texture: u8,
    pub texture_strength: f64,
    pub pressure_size: bool,
    pub pressure_flow: bool,
}
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentPaintLayer {
    pub coordinate_width: u32,
    pub coordinate_height: u32,
    pub blend: u8,
    pub opacity: f64,
    pub strokes: Vec<AdjustmentPaintStroke>,
}
pub(super) fn validate_paint(layer: &AdjustmentPaintLayer) -> Result<(), BridgeError> {
    let unit = |v: f64| v.is_finite() && (0.0..=1.0).contains(&v);
    let invalid = || BridgeError::InvalidEditRequest("invalid bounded paint layer");
    if layer.coordinate_width == 0
        || layer.coordinate_height == 0
        || layer.coordinate_width > 131_072
        || layer.coordinate_height > 131_072
        || layer.blend > 2
        || !unit(layer.opacity)
        || layer.strokes.len() > 128
    {
        return Err(invalid());
    }
    let mut total = 0;
    let mut dabs = 0.0;
    let short = f64::from(layer.coordinate_width.min(layer.coordinate_height));
    for s in &layer.strokes {
        total += s.points.len();
        if s.points.is_empty()
            || s.points.len() > 2048
            || total > 32768
            || !s.radius.is_finite()
            || !(0.0001..=0.25).contains(&s.radius)
            || !unit(s.hardness)
            || !unit(s.opacity)
            || !unit(s.flow)
            || !s.roundness.is_finite()
            || !(0.1..=1.0).contains(&s.roundness)
            || !s.angle_degrees.is_finite()
            || !(-180.0..=180.0).contains(&s.angle_degrees)
            || !s.spacing.is_finite()
            || !(0.02..=1.0).contains(&s.spacing)
            || s.texture > 2
            || !unit(s.texture_strength)
            || !s.color.iter().all(|v| unit(*v))
            || !s
                .points
                .iter()
                .all(|p| unit(p.x) && unit(p.y) && unit(p.pressure))
        {
            return Err(invalid());
        }
        let distance: f64 = s
            .points
            .windows(2)
            .map(|p| {
                ((p[1].x - p[0].x) * f64::from(layer.coordinate_width) / short)
                    .hypot((p[1].y - p[0].y) * f64::from(layer.coordinate_height) / short)
            })
            .sum();
        let count =
            1.0 + distance / (s.radius * 2.0 * s.spacing * if s.pressure_size { 0.1 } else { 1.0 });
        dabs += count;
        if count > 32_760.0 || dabs > 262_144.0 {
            return Err(invalid());
        }
    }
    Ok(())
}

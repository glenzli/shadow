//! Master and individual RGB curves in signed sRGB-encoded working primaries.

use super::ToneCurvePoint;

/// Four independent PCHIP point sets, applied master first, then R/G/B.
/// Extended values use tangent extrapolation without intermediate clipping.
#[derive(Debug, Clone, PartialEq)]
pub struct RgbToneCurves {
    pub channels: [Vec<ToneCurvePoint>; 4],
}

impl Default for RgbToneCurves {
    fn default() -> Self {
        Self {
            channels: std::array::from_fn(|_| {
                vec![
                    ToneCurvePoint { x: 0.0, y: 0.0 },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ]
            }),
        }
    }
}

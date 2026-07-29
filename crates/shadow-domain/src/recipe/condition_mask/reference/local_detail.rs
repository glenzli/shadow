//! Full-render scale, neighborhood footprint, and local-detail response.

use thiserror::Error;

use super::super::{
    LOCAL_DETAIL_RESIDUAL_NORMALIZATION, MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS,
};

/// Exact semantic linear scale of a complete render relative to level zero.
///
/// The ratio is authored by the render plan and is independent of a tile's
/// width, height, or origin. For example, a half-resolution full render uses
/// `new(1, 2)` for every tile. The ratio is kept rational so radius rounding
/// is identical across CPU and GPU planning.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct LocalDetailFullRenderScale {
    full_render_units: u32,
    level_zero_units: u32,
}

impl LocalDetailFullRenderScale {
    /// Creates a positive full-render scale no greater than level zero.
    ///
    /// # Errors
    ///
    /// Returns an error for a zero extent or an upscale ratio.
    pub fn new(
        full_render_units: u32,
        level_zero_units: u32,
    ) -> Result<Self, ConditionMaskReferenceError> {
        if full_render_units == 0 || level_zero_units == 0 || full_render_units > level_zero_units {
            return Err(ConditionMaskReferenceError::InvalidFullRenderScale {
                full_render_units,
                level_zero_units,
            });
        }
        Ok(Self {
            full_render_units,
            level_zero_units,
        })
    }

    pub const fn full_render_units(self) -> u32 {
        self.full_render_units
    }

    pub const fn level_zero_units(self) -> u32 {
        self.level_zero_units
    }

    /// Converts a level-zero radius using round-half-up and a one-pixel floor.
    ///
    /// The exact formula is
    /// `max(1, floor(radius * full_render_units / level_zero_units + 0.5))`.
    ///
    /// # Errors
    ///
    /// Returns an error if the radius is outside the persistent v1 bounds.
    pub fn scaled_radius(
        self,
        radius_level_zero_pixels: u16,
    ) -> Result<u16, ConditionMaskReferenceError> {
        if !(1..=MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS).contains(&radius_level_zero_pixels) {
            return Err(ConditionMaskReferenceError::InvalidLocalDetailRadius(
                radius_level_zero_pixels,
            ));
        }
        let twice_scaled =
            2_u64 * u64::from(radius_level_zero_pixels) * u64::from(self.full_render_units);
        let level_zero_units = u64::from(self.level_zero_units);
        let rounded = (twice_scaled + level_zero_units) / (2 * level_zero_units);
        u16::try_from(rounded.max(1)).map_err(|_| {
            ConditionMaskReferenceError::InvalidLocalDetailRadius(radius_level_zero_pixels)
        })
    }
}

/// Invalid input to the normative local-detail reference evaluator.
#[derive(Debug, Clone, Eq, PartialEq, Error)]
pub enum ConditionMaskReferenceError {
    #[error(
        "condition-mask full-render scale {full_render_units}/{level_zero_units} must be positive and no greater than one"
    )]
    InvalidFullRenderScale {
        full_render_units: u32,
        level_zero_units: u32,
    },
    #[error("condition-mask local-detail radius {0} is outside the persistent v1 bounds")]
    InvalidLocalDetailRadius(u16),
    #[error("condition-mask local-detail full-render dimensions must both be non-zero")]
    EmptyFullRender,
    #[error("condition-mask local-detail full-render dimensions overflow addressable storage")]
    FullRenderDimensionsOverflow,
    #[error(
        "condition-mask local-detail raster contains {actual} samples; expected exactly {expected}"
    )]
    FullRenderLengthMismatch { expected: usize, actual: usize },
    #[error(
        "condition-mask local-detail coordinate ({x}, {y}) lies outside {width}x{height} full render"
    )]
    CoordinateOutsideFullRender {
        x: u32,
        y: u32,
        width: u32,
        height: u32,
    },
    #[error("condition-mask local-detail Oklab lightness sample {0} is non-finite")]
    NonFiniteLightness(usize),
}

/// Computes the v1 local-detail response for one pixel of the complete render.
///
/// `full_render_lightness` is a tightly packed, row-major Oklab `L` raster for
/// the complete render, not merely the current tile. The scaled radius forms a
/// `(2r + 1) × (2r + 1)` square centered on `(x, y)`. Every out-of-bounds
/// coordinate is clamped independently to the nearest full-render edge, so
/// edge samples are repeated rather than shortening the box. The mean is
/// accumulated in row-major order. The response is
/// `clamp(abs(center_L - mean_L) / 0.25, 0, 1)`.
///
/// # Errors
///
/// Returns an error for invalid dimensions, sample count, coordinate, scale,
/// radius, or non-finite lightness.
pub fn local_detail_reference_response(
    full_render_lightness: &[f64],
    full_render_width: u32,
    full_render_height: u32,
    x: u32,
    y: u32,
    radius_level_zero_pixels: u16,
    full_render_scale: LocalDetailFullRenderScale,
) -> Result<f64, ConditionMaskReferenceError> {
    if full_render_width == 0 || full_render_height == 0 {
        return Err(ConditionMaskReferenceError::EmptyFullRender);
    }
    let expected_u64 = u64::from(full_render_width) * u64::from(full_render_height);
    let expected = usize::try_from(expected_u64)
        .map_err(|_| ConditionMaskReferenceError::FullRenderDimensionsOverflow)?;
    if full_render_lightness.len() != expected {
        return Err(ConditionMaskReferenceError::FullRenderLengthMismatch {
            expected,
            actual: full_render_lightness.len(),
        });
    }
    if x >= full_render_width || y >= full_render_height {
        return Err(ConditionMaskReferenceError::CoordinateOutsideFullRender {
            x,
            y,
            width: full_render_width,
            height: full_render_height,
        });
    }
    if let Some((index, _)) = full_render_lightness
        .iter()
        .enumerate()
        .find(|(_, value)| !value.is_finite())
    {
        return Err(ConditionMaskReferenceError::NonFiniteLightness(index));
    }

    let radius = i32::from(full_render_scale.scaled_radius(radius_level_zero_pixels)?);
    let mut sum = 0.0;
    for offset_y in -radius..=radius {
        let sample_y = clamped_coordinate(y, offset_y, full_render_height);
        for offset_x in -radius..=radius {
            let sample_x = clamped_coordinate(x, offset_x, full_render_width);
            let index = usize::try_from(
                u64::from(sample_y) * u64::from(full_render_width) + u64::from(sample_x),
            )
            .map_err(|_| ConditionMaskReferenceError::FullRenderDimensionsOverflow)?;
            sum += full_render_lightness[index];
        }
    }
    let side = f64::from(2 * radius + 1);
    let mean = sum / (side * side);
    let center_index = usize::try_from(u64::from(y) * u64::from(full_render_width) + u64::from(x))
        .map_err(|_| ConditionMaskReferenceError::FullRenderDimensionsOverflow)?;
    Ok(
        ((full_render_lightness[center_index] - mean).abs() / LOCAL_DETAIL_RESIDUAL_NORMALIZATION)
            .clamp(0.0, 1.0),
    )
}

fn clamped_coordinate(coordinate: u32, offset: i32, extent: u32) -> u32 {
    let clamped = (i64::from(coordinate) + i64::from(offset)).clamp(0, i64::from(extent - 1));
    match u32::try_from(clamped) {
        Ok(value) => value,
        Err(_) => unreachable!("clamped u32 coordinate must remain representable"),
    }
}

#[cfg(test)]
mod tests;

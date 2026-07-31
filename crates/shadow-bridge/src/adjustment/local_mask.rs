//! Normalized local-mask shapes and condition-range invariants.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// Maximum immutable raster payload admitted into one native render request.
pub const MAX_MANAGED_RASTER_MASK_BYTES: usize = 64 * 1024 * 1024;

/// Portable grayscale coverage encoding for a managed raster mask.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AdjustmentRasterMaskEncoding {
    Gray8,
    /// Tightly packed little-endian IEEE-754 binary16 samples.
    Gray16Float,
}

/// A normalized local mask ready for the native layer mixer.
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
    LuminanceRange {
        lower: f64,
        upper: f64,
        softness: f64,
        invert: bool,
    },
    ColorRange {
        center_hue_degrees: f64,
        width_degrees: f64,
        softness: f64,
        invert: bool,
    },
    /// Immutable coverage produced by an application-managed mask revision.
    /// The bridge receives already verified bytes; filesystem resolution and
    /// digest checks remain application-service responsibilities.
    ManagedRaster {
        raster_width: u32,
        raster_height: u32,
        coordinate_width: u32,
        coordinate_height: u32,
        encoding: AdjustmentRasterMaskEncoding,
        samples: Vec<u8>,
        /// Signed authoring control in `[-1, 1]`; runtime maps its magnitude
        /// to a bounded fraction of the stored raster's shorter edge.
        expansion: f64,
        /// Symmetric edge softening in `[0, 1]`.
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

fn validate_normalized_mask_value(value: f64) -> Result<(), BridgeError> {
    validate_finite_render_parameter(value)?;
    if (0.0..=1.0).contains(&value) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "local-mask coordinates must be normalized to 0..=1",
        ))
    }
}

fn validate_managed_raster_mask(
    dimensions: [u32; 4],
    encoding: AdjustmentRasterMaskEncoding,
    samples: &[u8],
) -> Result<(), BridgeError> {
    let [
        raster_width,
        raster_height,
        coordinate_width,
        coordinate_height,
    ] = dimensions;
    if raster_width == 0 || raster_height == 0 || coordinate_width == 0 || coordinate_height == 0 {
        return Err(BridgeError::InvalidEditRequest(
            "managed raster mask dimensions and coordinate extent must be non-zero",
        ));
    }
    let sample_count = usize::try_from(raster_width)
        .ok()
        .and_then(|width| {
            usize::try_from(raster_height)
                .ok()
                .and_then(|height| width.checked_mul(height))
        })
        .ok_or(BridgeError::InvalidEditRequest(
            "managed raster mask dimensions exceed the host address space",
        ))?;
    let bytes_per_sample = match encoding {
        AdjustmentRasterMaskEncoding::Gray8 => 1,
        AdjustmentRasterMaskEncoding::Gray16Float => 2,
    };
    let expected_bytes =
        sample_count
            .checked_mul(bytes_per_sample)
            .ok_or(BridgeError::InvalidEditRequest(
                "managed raster mask byte count overflows the host address space",
            ))?;
    if expected_bytes > MAX_MANAGED_RASTER_MASK_BYTES {
        return Err(BridgeError::InvalidEditRequest(
            "managed raster mask exceeds the 64 MiB execution budget",
        ));
    }
    if samples.len() != expected_bytes {
        return Err(BridgeError::InvalidEditRequest(
            "managed raster mask payload is not tightly packed for its dimensions and encoding",
        ));
    }
    if encoding == AdjustmentRasterMaskEncoding::Gray16Float
        && samples
            .chunks_exact(2)
            .any(|bytes| u16::from_le_bytes([bytes[0], bytes[1]]) > 0x3c00)
    {
        return Err(BridgeError::InvalidEditRequest(
            "managed raster mask binary16 samples must be finite and in 0..=1",
        ));
    }
    Ok(())
}

fn validate_color_range(
    center_hue_degrees: f64,
    width_degrees: f64,
    softness: f64,
) -> Result<(), BridgeError> {
    for value in [center_hue_degrees, width_degrees, softness] {
        validate_finite_render_parameter(value)?;
    }
    if !(0.0..360.0).contains(&center_hue_degrees) {
        return Err(BridgeError::InvalidEditRequest(
            "local-mask color hue must use the canonical interval 0..360",
        ));
    }
    if !(1.0..=180.0).contains(&width_degrees) {
        return Err(BridgeError::InvalidEditRequest(
            "local-mask color width must be in 1..=180 degrees",
        ));
    }
    validate_normalized_mask_value(softness)
}

// Keep every variant's public-wire invariant in one exhaustive match so a new
// mask kind cannot bypass validation by being omitted from a secondary router.
#[allow(clippy::too_many_lines)]
pub(super) fn validate_adjustment_local_mask(
    mask: &AdjustmentLocalMask,
) -> Result<(), BridgeError> {
    match mask {
        AdjustmentLocalMask::LinearGradient {
            start_x,
            start_y,
            end_x,
            end_y,
            ..
        } => {
            for value in [*start_x, *start_y, *end_x, *end_y] {
                validate_normalized_mask_value(value)?;
            }
            let (dx, dy) = (*end_x - *start_x, *end_y - *start_y);
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
                validate_normalized_mask_value(value)?;
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
                validate_normalized_mask_value(value)?;
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
                validate_normalized_mask_value(point.x)?;
                validate_normalized_mask_value(point.y)?;
            }
        }
        AdjustmentLocalMask::LuminanceRange {
            lower,
            upper,
            softness,
            ..
        } => {
            for value in [*lower, *upper, *softness] {
                validate_normalized_mask_value(value)?;
            }
            if lower > upper {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask luminance lower bound must not exceed its upper bound",
                ));
            }
        }
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees,
            width_degrees,
            softness,
            ..
        } => validate_color_range(*center_hue_degrees, *width_degrees, *softness)?,
        AdjustmentLocalMask::ManagedRaster {
            raster_width,
            raster_height,
            coordinate_width,
            coordinate_height,
            encoding,
            samples,
            expansion,
            feather,
            ..
        } => {
            validate_finite_render_parameter(*expansion)?;
            if !(-1.0..=1.0).contains(expansion) {
                return Err(BridgeError::InvalidEditRequest(
                    "managed raster mask expansion must be in -1..=1",
                ));
            }
            validate_normalized_mask_value(*feather)?;
            validate_managed_raster_mask(
                [
                    *raster_width,
                    *raster_height,
                    *coordinate_width,
                    *coordinate_height,
                ],
                *encoding,
                samples,
            )?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;

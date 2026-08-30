//! Bounded accepted AI-completion patch rendering contract.

use super::{BridgeError, parameter_validation::validate_finite_render_parameter};

pub const MAX_ADJUSTMENT_IMAGE_COMPLETION_PATCHES: usize = 32;
pub const MAX_ADJUSTMENT_IMAGE_COMPLETION_EDGE: u32 = 2_048;
pub const MAX_ADJUSTMENT_IMAGE_COMPLETION_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentImageCompletionPatch {
    pub raster_width: u32,
    pub raster_height: u32,
    pub coordinate_width: u32,
    pub coordinate_height: u32,
    pub bounds_left: f64,
    pub bounds_top: f64,
    pub bounds_right: f64,
    pub bounds_bottom: f64,
    pub strength: f64,
    pub rgba8: Vec<u8>,
}

pub(super) fn validate_image_completion(
    patches: &[AdjustmentImageCompletionPatch],
) -> Result<(), BridgeError> {
    if patches.is_empty() || patches.len() > MAX_ADJUSTMENT_IMAGE_COMPLETION_PATCHES {
        return Err(BridgeError::InvalidEditRequest(
            "AI completion must contain 1 through 32 accepted patches",
        ));
    }
    for patch in patches {
        if patch.raster_width == 0
            || patch.raster_height == 0
            || patch.raster_width > MAX_ADJUSTMENT_IMAGE_COMPLETION_EDGE
            || patch.raster_height > MAX_ADJUSTMENT_IMAGE_COMPLETION_EDGE
            || patch.coordinate_width == 0
            || patch.coordinate_height == 0
        {
            return Err(BridgeError::InvalidEditRequest(
                "AI completion patch extent is outside the supported range",
            ));
        }
        let expected = usize::try_from(patch.raster_width)
            .ok()
            .and_then(|width| {
                usize::try_from(patch.raster_height)
                    .ok()
                    .and_then(|height| width.checked_mul(height))
            })
            .and_then(|pixels| pixels.checked_mul(4))
            .ok_or(BridgeError::InvalidEditRequest(
                "AI completion patch byte length overflowed",
            ))?;
        if expected > MAX_ADJUSTMENT_IMAGE_COMPLETION_BYTES || patch.rgba8.len() != expected {
            return Err(BridgeError::InvalidEditRequest(
                "AI completion patch must be bounded tightly packed RGBA8",
            ));
        }
        for value in [
            patch.bounds_left,
            patch.bounds_top,
            patch.bounds_right,
            patch.bounds_bottom,
            patch.strength,
        ] {
            validate_finite_render_parameter(value)?;
        }
        if !(0.0..1.0).contains(&patch.bounds_left)
            || !(0.0..1.0).contains(&patch.bounds_top)
            || !(0.0..=1.0).contains(&patch.bounds_right)
            || !(0.0..=1.0).contains(&patch.bounds_bottom)
            || patch.bounds_left >= patch.bounds_right
            || patch.bounds_top >= patch.bounds_bottom
            || !(0.0..=1.0).contains(&patch.strength)
        {
            return Err(BridgeError::InvalidEditRequest(
                "AI completion placement or strength is outside the normalized range",
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;

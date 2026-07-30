//! Photo-level crop, orientation, and output-canvas geometry.

use shadow_domain::ImageDimensions;

use crate::error::BridgeError;

/// Lossless right-angle orientation for the photo-level final canvas.
///
/// This remains separate from adjustment operations because it changes the output raster geometry
/// rather than mutating samples in the current raster.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum AdjustmentQuarterTurn {
    Zero,
    Clockwise90,
    Clockwise180,
    Clockwise270,
}

/// A bounded photo-level crop/orientation request ready for native rendering.
///
/// Crop values are original-image edge coordinates. The native kernel and the Rust detail
/// scheduler both turn those normalized edges into exactly the same pixel-aligned canvas before
/// they schedule tiles.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentGeometry {
    pub crop_left: f64,
    pub crop_top: f64,
    pub crop_right: f64,
    pub crop_bottom: f64,
    pub quarter_turn: AdjustmentQuarterTurn,
    pub straighten_degrees: f64,
    pub flip_horizontal: bool,
    pub flip_vertical: bool,
}

impl Default for AdjustmentGeometry {
    fn default() -> Self {
        Self::identity()
    }
}

impl AdjustmentGeometry {
    #[must_use]
    pub const fn identity() -> Self {
        Self {
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: AdjustmentQuarterTurn::Zero,
            straighten_degrees: 0.0,
            flip_horizontal: false,
            flip_vertical: false,
        }
    }

    #[must_use]
    #[allow(clippy::float_cmp)] // Identity is an exact serialized contract, not a measurement.
    pub const fn is_identity(self) -> bool {
        self.crop_left == 0.0
            && self.crop_top == 0.0
            && self.crop_right == 1.0
            && self.crop_bottom == 1.0
            && matches!(self.quarter_turn, AdjustmentQuarterTurn::Zero)
            && self.straighten_degrees == 0.0
            && !self.flip_horizontal
            && !self.flip_vertical
    }

    pub(super) fn validate(self) -> Result<(), BridgeError> {
        let values = [
            self.crop_left,
            self.crop_top,
            self.crop_right,
            self.crop_bottom,
        ];
        if values
            .iter()
            .any(|value| !value.is_finite() || !(0.0..=1.0).contains(value))
        {
            return Err(BridgeError::InvalidEditRequest(
                "photo geometry crop edges must be finite and normalized to 0..=1",
            ));
        }
        if self.crop_left >= self.crop_right || self.crop_top >= self.crop_bottom {
            return Err(BridgeError::InvalidEditRequest(
                "photo geometry crop must retain non-zero width and height",
            ));
        }
        if !self.straighten_degrees.is_finite()
            || !(-45.0..=45.0).contains(&self.straighten_degrees)
        {
            return Err(BridgeError::InvalidEditRequest(
                "photo geometry straighten angle must be in -45..=45 degrees",
            ));
        }
        Ok(())
    }

    /// Computes the exact pixel canvas used by geometry-aware detail tiles.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for malformed geometry, zero-sized source
    /// dimensions, or a crop that contains no addressable source pixels.
    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        clippy::float_cmp
    )] // Values are clamped first; zero is the exact persisted no-straighten value.
    pub fn output_dimensions(
        self,
        source: ImageDimensions,
    ) -> Result<ImageDimensions, BridgeError> {
        self.validate()?;
        let crop_axis = |source_extent: u32, lower: f64, upper: f64| {
            if source_extent == 0 {
                return Err(BridgeError::InvalidEditRequest(
                    "photo geometry requires non-zero source dimensions",
                ));
            }
            let extent = f64::from(source_extent);
            let lower = (lower * extent).floor().clamp(0.0, extent - 1.0) as u32;
            let upper = (upper * extent).ceil().clamp(1.0, extent) as u32;
            upper.checked_sub(lower).filter(|value| *value > 0).ok_or(
                BridgeError::InvalidEditRequest(
                    "photo geometry crop has no addressable source pixels",
                ),
            )
        };
        let width = crop_axis(source.width, self.crop_left, self.crop_right)?;
        let height = crop_axis(source.height, self.crop_top, self.crop_bottom)?;
        let (width, height) = match self.quarter_turn {
            AdjustmentQuarterTurn::Zero | AdjustmentQuarterTurn::Clockwise180 => (width, height),
            AdjustmentQuarterTurn::Clockwise90 | AdjustmentQuarterTurn::Clockwise270 => {
                (height, width)
            }
        };
        if self.straighten_degrees == 0.0 {
            return Ok(ImageDimensions { width, height });
        }

        // Keep this formula and its floor rounding identical to
        // cpp/shadow-image/src/edit/photo_geometry.cpp. Fine straighten
        // rotates around the crop centre, then shrinks the output while
        // preserving the crop's aspect ratio until every output corner maps
        // to a real source pixel.
        let radians = self.straighten_degrees.to_radians();
        let cosine = radians.cos().abs();
        let sine = radians.sin().abs();
        let width_f64 = f64::from(width);
        let height_f64 = f64::from(height);
        let scale = (width_f64 / (cosine * width_f64 + sine * height_f64))
            .min(height_f64 / (sine * width_f64 + cosine * height_f64));
        let retained_extent = |value: f64| value.floor().max(1.0) as u32;
        Ok(ImageDimensions {
            width: retained_extent(width_f64 * scale),
            height: retained_extent(height_f64 * scale),
        })
    }
}

#[cfg(test)]
mod tests;

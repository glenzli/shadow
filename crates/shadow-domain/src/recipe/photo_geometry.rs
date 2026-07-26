//! Photo-local crop, orientation, mirror, and straighten recipe contracts.

use serde::{Deserialize, Serialize};

use super::{FiniteF64, RecipeValidationError, UnitInterval, default_finite_zero};

/// A lossless 90-degree orientation applied after the photo's local edits.
///
/// Geometry is deliberately photo-local: a reusable Grade Node may describe a
/// colour treatment, but it must never silently crop or rotate every other
/// photograph that happens to reuse it.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PhotoQuarterTurn {
    Zero,
    Clockwise90,
    Clockwise180,
    Clockwise270,
}

/// The first persistent, non-destructive geometry contract.
///
/// Crop edges live in original-image *edge* coordinates.  They are therefore
/// independent of the proxy size used to show the photo.  A renderer turns
/// them into one pixel-aligned source rectangle at its current resolution;
/// right-angle orientation and flips then rearrange those pixels without an
/// additional resampling pass. Fine straighten remains in this same
/// photo-level contract and uses one final geometry resampling pass.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct PhotoGeometry {
    crop_left: UnitInterval,
    crop_top: UnitInterval,
    crop_right: UnitInterval,
    crop_bottom: UnitInterval,
    quarter_turn: PhotoQuarterTurn,
    #[serde(default = "default_finite_zero")]
    straighten_degrees: FiniteF64,
    flip_horizontal: bool,
    flip_vertical: bool,
}

impl Default for PhotoGeometry {
    fn default() -> Self {
        Self::identity()
    }
}

impl PhotoGeometry {
    /// Returns the exact original-image canvas without any orientation change.
    pub const fn identity() -> Self {
        Self {
            crop_left: UnitInterval::ZERO,
            crop_top: UnitInterval::ZERO,
            crop_right: UnitInterval::ONE,
            crop_bottom: UnitInterval::ONE,
            quarter_turn: PhotoQuarterTurn::Zero,
            straighten_degrees: default_finite_zero(),
            flip_horizontal: false,
            flip_vertical: false,
        }
    }

    /// Creates a validated photo-local crop/orientation state.
    ///
    /// The crop must retain non-zero extent on both axes.  Pixel-level
    /// clamping remains the renderer's responsibility because it alone knows
    /// the original raster dimensions.
    pub const fn new(
        crop_left: UnitInterval,
        crop_top: UnitInterval,
        crop_right: UnitInterval,
        crop_bottom: UnitInterval,
        quarter_turn: PhotoQuarterTurn,
        flip_horizontal: bool,
        flip_vertical: bool,
    ) -> Result<Self, RecipeValidationError> {
        if crop_left.get() >= crop_right.get() || crop_top.get() >= crop_bottom.get() {
            return Err(RecipeValidationError::DegeneratePhotoCrop);
        }
        Ok(Self {
            crop_left,
            crop_top,
            crop_right,
            crop_bottom,
            quarter_turn,
            straighten_degrees: default_finite_zero(),
            flip_horizontal,
            flip_vertical,
        })
    }

    /// Adds a fine clockwise straighten rotation in the bounded range used by
    /// professional crop tools. It remains part of the same Recipe v1
    /// geometry contract while Shadow is pre-release.
    pub fn with_straighten_degrees(mut self, degrees: f64) -> Result<Self, RecipeValidationError> {
        let degrees = FiniteF64::new(degrees)?;
        if !(-45.0..=45.0).contains(&degrees.get()) {
            return Err(RecipeValidationError::InvalidPhotoStraightenDegrees(
                degrees.get(),
            ));
        }
        self.straighten_degrees = degrees;
        Ok(self)
    }

    pub const fn crop_left(self) -> UnitInterval {
        self.crop_left
    }

    pub const fn crop_top(self) -> UnitInterval {
        self.crop_top
    }

    pub const fn crop_right(self) -> UnitInterval {
        self.crop_right
    }

    pub const fn crop_bottom(self) -> UnitInterval {
        self.crop_bottom
    }

    pub const fn quarter_turn(self) -> PhotoQuarterTurn {
        self.quarter_turn
    }

    pub const fn straighten_degrees(self) -> f64 {
        self.straighten_degrees.get()
    }

    pub const fn flip_horizontal(self) -> bool {
        self.flip_horizontal
    }

    pub const fn flip_vertical(self) -> bool {
        self.flip_vertical
    }

    pub const fn is_identity(&self) -> bool {
        self.crop_left.get() == 0.0
            && self.crop_top.get() == 0.0
            && self.crop_right.get() == 1.0
            && self.crop_bottom.get() == 1.0
            && matches!(self.quarter_turn, PhotoQuarterTurn::Zero)
            && self.straighten_degrees.get() == 0.0
            && !self.flip_horizontal
            && !self.flip_vertical
    }

    pub(super) fn validate(self) -> Result<(), RecipeValidationError> {
        let normalized = Self::new(
            self.crop_left,
            self.crop_top,
            self.crop_right,
            self.crop_bottom,
            self.quarter_turn,
            self.flip_horizontal,
            self.flip_vertical,
        )?;
        normalized
            .with_straighten_degrees(self.straighten_degrees.get())
            .map(|_| ())
    }
}

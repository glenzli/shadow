//! Photo-local crop, orientation, mirror, straighten, perspective, and final-canvas contracts.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;
use super::value::{FiniteF64, UnitInterval, default_finite_zero};

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
    #[serde(default = "default_finite_zero")]
    perspective_vertical: FiniteF64,
    #[serde(default = "default_finite_zero")]
    perspective_horizontal: FiniteF64,
    flip_horizontal: bool,
    flip_vertical: bool,
}

/// The optional, photo-local final-canvas node.
///
/// Its execution slot and order are fixed even though the node is absent from
/// a new Recipe's visible processing stack. Adding an identity crop records
/// `present`; bypassing records `enabled = false` while retaining every
/// authored geometry value. The role has no user-generated node id and can
/// never be duplicated, reordered, masked, or shared as a Grade Node.
///
/// Flattening retains the existing Recipe v1 `geometry` object while adding
/// the two lifecycle fields beside its crop/orientation parameters.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct PhotoCanvasNode {
    #[serde(default, skip_serializing_if = "bool_is_false")]
    present: bool,
    #[serde(default = "bool_true", skip_serializing_if = "bool_is_true")]
    enabled: bool,
    #[serde(flatten)]
    geometry: PhotoGeometry,
}

impl Default for PhotoCanvasNode {
    fn default() -> Self {
        Self::identity()
    }
}

impl PhotoCanvasNode {
    /// Returns the canonical absent Canvas node.
    pub const fn identity() -> Self {
        Self {
            present: false,
            enabled: true,
            geometry: PhotoGeometry::identity(),
        }
    }

    /// Adds the singleton Canvas node with one validated authored geometry.
    pub const fn new(geometry: PhotoGeometry) -> Self {
        Self {
            present: true,
            enabled: true,
            geometry,
        }
    }

    /// Adds an identity Canvas ready for direct crop/geometry authoring.
    pub const fn added() -> Self {
        Self::new(PhotoGeometry::identity())
    }

    #[must_use]
    pub const fn with_present(mut self, present: bool) -> Self {
        self.present = present;
        if !present {
            self.enabled = true;
            self.geometry = PhotoGeometry::identity();
        }
        self
    }

    #[must_use]
    pub const fn with_enabled(mut self, enabled: bool) -> Self {
        if self.is_present() {
            self.enabled = enabled;
        }
        self
    }

    /// Older Recipe v1 geometry payloads predate `present`; non-identity
    /// authored geometry therefore remains authoritative evidence of presence.
    pub const fn is_present(&self) -> bool {
        self.present || !self.geometry.is_identity()
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    /// Returns the crop/orientation parameters owned by this node.
    pub const fn geometry(self) -> PhotoGeometry {
        self.geometry
    }

    /// Returns the geometry that may participate in rendering.
    pub const fn effective_geometry(self) -> PhotoGeometry {
        if self.is_present() && self.enabled {
            self.geometry
        } else {
            PhotoGeometry::identity()
        }
    }

    /// Returns whether the optional Canvas node is canonically absent.
    pub const fn is_identity(&self) -> bool {
        !self.is_present() && self.enabled && self.geometry.is_identity()
    }

    pub(super) fn validate(self) -> Result<(), RecipeValidationError> {
        self.geometry.validate()
    }
}

const fn bool_true() -> bool {
    true
}

#[allow(clippy::trivially_copy_pass_by_ref)]
const fn bool_is_true(value: &bool) -> bool {
    *value
}

#[allow(clippy::trivially_copy_pass_by_ref)]
const fn bool_is_false(value: &bool) -> bool {
    !*value
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
            perspective_vertical: default_finite_zero(),
            perspective_horizontal: default_finite_zero(),
            flip_horizontal: false,
            flip_vertical: false,
        }
    }

    /// Creates a validated photo-local crop/orientation state.
    ///
    /// The crop must retain non-zero extent on both axes.  Pixel-level
    /// clamping remains the renderer's responsibility because it alone knows
    /// the original raster dimensions.
    ///
    /// # Errors
    ///
    /// Returns an error when either crop axis has zero or negative extent.
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
            perspective_vertical: default_finite_zero(),
            perspective_horizontal: default_finite_zero(),
            flip_horizontal,
            flip_vertical,
        })
    }

    /// Adds a fine clockwise straighten rotation in the bounded range used by
    /// professional crop tools. It remains part of the same Recipe v1
    /// geometry contract while Shadow is pre-release.
    ///
    /// # Errors
    ///
    /// Returns an error when `degrees` is non-finite or outside `[-45, 45]`.
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

    /// Applies bounded symmetric keystone correction in the oriented photo
    /// coordinate system. The renderer maps the output rectangle into an
    /// interior trapezoid, so correction never introduces synthetic corners.
    ///
    /// # Errors
    ///
    /// Returns an error when either axis is non-finite or outside `[-1, 1]`.
    pub fn with_perspective(
        mut self,
        vertical: f64,
        horizontal: f64,
    ) -> Result<Self, RecipeValidationError> {
        let vertical = FiniteF64::new(vertical)?;
        let horizontal = FiniteF64::new(horizontal)?;
        for (axis, value) in [
            ("vertical", vertical.get()),
            ("horizontal", horizontal.get()),
        ] {
            if !(-1.0..=1.0).contains(&value) {
                return Err(RecipeValidationError::InvalidPhotoPerspective { axis, value });
            }
        }
        self.perspective_vertical = vertical;
        self.perspective_horizontal = horizontal;
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

    pub const fn perspective_vertical(self) -> f64 {
        self.perspective_vertical.get()
    }

    pub const fn perspective_horizontal(self) -> f64 {
        self.perspective_horizontal.get()
    }

    pub const fn flip_horizontal(self) -> bool {
        self.flip_horizontal
    }

    pub const fn flip_vertical(self) -> bool {
        self.flip_vertical
    }

    #[allow(clippy::float_cmp)]
    pub const fn is_identity(&self) -> bool {
        self.crop_left.get() == 0.0
            && self.crop_top.get() == 0.0
            && self.crop_right.get() == 1.0
            && self.crop_bottom.get() == 1.0
            && matches!(self.quarter_turn, PhotoQuarterTurn::Zero)
            && self.straighten_degrees.get() == 0.0
            && self.perspective_vertical.get() == 0.0
            && self.perspective_horizontal.get() == 0.0
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
            .and_then(|value| {
                value.with_perspective(
                    self.perspective_vertical.get(),
                    self.perspective_horizontal.get(),
                )
            })
            .map(|_| ())
    }
}

#[cfg(test)]
mod tests;

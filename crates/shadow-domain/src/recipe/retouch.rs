//! Photo-local, non-generative repair and clone recipe contracts.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;
use super::value::{FiniteF64, UnitInterval, default_finite_zero};

/// Deterministic non-generative repair behavior.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RetouchMode {
    /// Preserve nearby donor texture while matching the target boundary.
    #[default]
    Heal,
    /// Copy from a nearby source offset measured in brush radii.
    Clone,
}

const fn default_retouch_feather() -> UnitInterval {
    UnitInterval(FiniteF64(0.28))
}

const fn default_retouch_strength() -> UnitInterval {
    UnitInterval(FiniteF64(1.0))
}

/// One small, non-generative repair target in original-image coordinates.
///
/// Radius is expressed in level-zero pixels. Clone offsets are measured in
/// radii and deliberately bounded, preserving tile-local detail execution.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct RetouchSpot {
    center_x: UnitInterval,
    center_y: UnitInterval,
    radius_level_zero_pixels: u16,
    #[serde(default)]
    mode: RetouchMode,
    #[serde(default = "default_finite_zero")]
    source_offset_x_radii: FiniteF64,
    #[serde(default = "default_finite_zero")]
    source_offset_y_radii: FiniteF64,
    #[serde(default = "default_retouch_feather")]
    feather: UnitInterval,
    #[serde(default = "default_retouch_strength")]
    strength: UnitInterval,
}

impl RetouchSpot {
    pub const MIN_RADIUS_LEVEL_ZERO_PIXELS: u16 = 1;
    pub const MAX_RADIUS_LEVEL_ZERO_PIXELS: u16 = 128;

    /// Creates a bounded dust-spot repair target.
    ///
    /// # Errors
    ///
    /// Returns an error when the radius cannot fit within Shadow's bounded
    /// detail-tile apron contract.
    pub const fn new(
        center_x: UnitInterval,
        center_y: UnitInterval,
        radius_level_zero_pixels: u16,
    ) -> Result<Self, RecipeValidationError> {
        if radius_level_zero_pixels < Self::MIN_RADIUS_LEVEL_ZERO_PIXELS
            || radius_level_zero_pixels > Self::MAX_RADIUS_LEVEL_ZERO_PIXELS
        {
            return Err(RecipeValidationError::InvalidRetouchSpotRadius(
                radius_level_zero_pixels,
            ));
        }
        Ok(Self {
            center_x,
            center_y,
            radius_level_zero_pixels,
            mode: RetouchMode::Heal,
            source_offset_x_radii: default_finite_zero(),
            source_offset_y_radii: default_finite_zero(),
            feather: default_retouch_feather(),
            strength: default_retouch_strength(),
        })
    }

    /// Applies the repair algorithm, source displacement, and edge feather.
    ///
    /// # Errors
    ///
    /// Returns an error when either source displacement is non-finite or
    /// outside the supported `[-8, 8]` radius range.
    pub fn with_behavior(
        mut self,
        mode: RetouchMode,
        horizontal_source_offset_radii: f64,
        vertical_source_offset_radii: f64,
        feather: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let horizontal_offset = FiniteF64::new(horizontal_source_offset_radii)?;
        let vertical_offset = FiniteF64::new(vertical_source_offset_radii)?;
        if !(-8.0..=8.0).contains(&horizontal_offset.get())
            || !(-8.0..=8.0).contains(&vertical_offset.get())
        {
            return Err(RecipeValidationError::InvalidRetouchSourceOffset);
        }
        self.mode = mode;
        self.source_offset_x_radii = horizontal_offset;
        self.source_offset_y_radii = vertical_offset;
        self.feather = feather;
        Ok(self)
    }

    /// Sets how strongly the completed repair is blended over the original.
    #[must_use]
    pub const fn with_strength(mut self, strength: UnitInterval) -> Self {
        self.strength = strength;
        self
    }

    pub const fn center_x(self) -> UnitInterval {
        self.center_x
    }

    pub const fn center_y(self) -> UnitInterval {
        self.center_y
    }

    pub const fn radius_level_zero_pixels(self) -> u16 {
        self.radius_level_zero_pixels
    }

    pub const fn mode(self) -> RetouchMode {
        self.mode
    }

    pub const fn source_offset_x_radii(self) -> f64 {
        self.source_offset_x_radii.get()
    }

    pub const fn source_offset_y_radii(self) -> f64 {
        self.source_offset_y_radii.get()
    }

    pub const fn feather(self) -> UnitInterval {
        self.feather
    }

    pub const fn strength(self) -> UnitInterval {
        self.strength
    }

    pub(super) fn validate(self) -> Result<(), RecipeValidationError> {
        Self::new(self.center_x, self.center_y, self.radius_level_zero_pixels)?
            .with_behavior(
                self.mode,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                self.feather,
            )
            .map(|value| value.with_strength(self.strength))
            .map(|_| ())
    }
}

pub const MAX_RETOUCH_SPOTS_PER_RECIPE: usize = 64;

/// One normalized centerline sample belonging to a continuous repair stroke.
///
/// The renderer sweeps the stroke radius along the ordered samples. A
/// one-point stroke is intentionally valid and represents a circular dab;
/// callers that model a drag normally provide two or more points.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct RetouchPoint {
    x: UnitInterval,
    y: UnitInterval,
}

impl RetouchPoint {
    pub const fn new(x: UnitInterval, y: UnitInterval) -> Self {
        Self { x, y }
    }

    pub const fn x(self) -> UnitInterval {
        self.x
    }

    pub const fn y(self) -> UnitInterval {
        self.y
    }
}

/// One photo-local continuous repair or clone brush stroke.
///
/// A stroke owns one ordered path, one radius, and one source offset. This
/// preserves the user's single-stroke/one-undo semantic while allowing the
/// renderer to rasterize the path as a continuous capsule union rather than a
/// collection of independently editable circular spots.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RetouchStroke {
    points: Vec<RetouchPoint>,
    radius_level_zero_pixels: u16,
    #[serde(default)]
    mode: RetouchMode,
    #[serde(default = "default_finite_zero")]
    source_offset_x_radii: FiniteF64,
    #[serde(default = "default_finite_zero")]
    source_offset_y_radii: FiniteF64,
    #[serde(default = "default_retouch_feather")]
    feather: UnitInterval,
    #[serde(default = "default_retouch_strength")]
    strength: UnitInterval,
}

impl RetouchStroke {
    pub const MIN_RADIUS_LEVEL_ZERO_PIXELS: u16 = RetouchSpot::MIN_RADIUS_LEVEL_ZERO_PIXELS;
    pub const MAX_RADIUS_LEVEL_ZERO_PIXELS: u16 = RetouchSpot::MAX_RADIUS_LEVEL_ZERO_PIXELS;

    /// Creates a bounded continuous repair stroke in original-image
    /// coordinates.
    ///
    /// # Errors
    ///
    /// Returns an error when the path is empty, has too many samples, or its
    /// radius exceeds Shadow's detail-tile apron contract.
    pub fn new(
        points: Vec<RetouchPoint>,
        radius_level_zero_pixels: u16,
    ) -> Result<Self, RecipeValidationError> {
        if points.is_empty() {
            return Err(RecipeValidationError::EmptyRetouchStroke);
        }
        if points.len() > MAX_RETOUCH_STROKE_POINTS {
            return Err(RecipeValidationError::TooManyRetouchStrokePoints(
                points.len(),
            ));
        }
        if !(Self::MIN_RADIUS_LEVEL_ZERO_PIXELS..=Self::MAX_RADIUS_LEVEL_ZERO_PIXELS)
            .contains(&radius_level_zero_pixels)
        {
            return Err(RecipeValidationError::InvalidRetouchStrokeRadius(
                radius_level_zero_pixels,
            ));
        }
        Ok(Self {
            points,
            radius_level_zero_pixels,
            mode: RetouchMode::Heal,
            source_offset_x_radii: default_finite_zero(),
            source_offset_y_radii: default_finite_zero(),
            feather: default_retouch_feather(),
            strength: default_retouch_strength(),
        })
    }

    /// Applies the repair algorithm, shared source displacement, and edge
    /// feather to the complete continuous stroke.
    ///
    /// # Errors
    ///
    /// Returns an error when either source displacement is non-finite or
    /// outside the supported `[-8, 8]` radius range.
    pub fn with_behavior(
        mut self,
        mode: RetouchMode,
        horizontal_source_offset_radii: f64,
        vertical_source_offset_radii: f64,
        feather: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let horizontal_offset = FiniteF64::new(horizontal_source_offset_radii)?;
        let vertical_offset = FiniteF64::new(vertical_source_offset_radii)?;
        if !(-8.0..=8.0).contains(&horizontal_offset.get())
            || !(-8.0..=8.0).contains(&vertical_offset.get())
        {
            return Err(RecipeValidationError::InvalidRetouchSourceOffset);
        }
        self.mode = mode;
        self.source_offset_x_radii = horizontal_offset;
        self.source_offset_y_radii = vertical_offset;
        self.feather = feather;
        Ok(self)
    }

    /// Sets how strongly the completed repair is blended over the original.
    #[must_use]
    pub const fn with_strength(mut self, strength: UnitInterval) -> Self {
        self.strength = strength;
        self
    }

    pub fn points(&self) -> &[RetouchPoint] {
        &self.points
    }

    pub const fn radius_level_zero_pixels(&self) -> u16 {
        self.radius_level_zero_pixels
    }

    pub const fn mode(&self) -> RetouchMode {
        self.mode
    }

    pub const fn source_offset_x_radii(&self) -> f64 {
        self.source_offset_x_radii.get()
    }

    pub const fn source_offset_y_radii(&self) -> f64 {
        self.source_offset_y_radii.get()
    }

    pub const fn feather(&self) -> UnitInterval {
        self.feather
    }

    pub const fn strength(&self) -> UnitInterval {
        self.strength
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(self.points.clone(), self.radius_level_zero_pixels)?
            .with_behavior(
                self.mode,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                self.feather,
            )
            .map(|value| value.with_strength(self.strength))
            .map(|_| ())
    }
}

/// A recipe limits the number of independently editable repair strokes just
/// as it limits legacy circular repair spots.
pub const MAX_RETOUCH_STROKES_PER_RECIPE: usize = 64;
/// One drag may retain at most this many normalized centerline samples.
pub const MAX_RETOUCH_STROKE_POINTS: usize = 512;

#[cfg(test)]
mod tests;

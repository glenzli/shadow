//! Photo-local, non-generative repair and clone recipe contracts.

use serde::{Deserialize, Serialize};

use super::{FiniteF64, RecipeValidationError, UnitInterval, default_finite_zero};

/// Deterministic non-generative repair behavior.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RetouchMode {
    /// Reconstruct from a smooth ring surrounding the selected defect.
    #[default]
    Heal,
    /// Copy from a nearby source offset measured in brush radii.
    Clone,
}

const fn default_retouch_feather() -> UnitInterval {
    UnitInterval(FiniteF64(0.28))
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
        })
    }

    pub fn with_behavior(
        mut self,
        mode: RetouchMode,
        source_offset_x_radii: f64,
        source_offset_y_radii: f64,
        feather: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let source_offset_x_radii = FiniteF64::new(source_offset_x_radii)?;
        let source_offset_y_radii = FiniteF64::new(source_offset_y_radii)?;
        if !(-2.0..=2.0).contains(&source_offset_x_radii.get())
            || !(-2.0..=2.0).contains(&source_offset_y_radii.get())
        {
            return Err(RecipeValidationError::InvalidRetouchSourceOffset);
        }
        self.mode = mode;
        self.source_offset_x_radii = source_offset_x_radii;
        self.source_offset_y_radii = source_offset_y_radii;
        self.feather = feather;
        Ok(self)
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

    pub(super) fn validate(self) -> Result<(), RecipeValidationError> {
        Self::new(self.center_x, self.center_y, self.radius_level_zero_pixels)?
            .with_behavior(
                self.mode,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                self.feather,
            )
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
        if radius_level_zero_pixels < Self::MIN_RADIUS_LEVEL_ZERO_PIXELS
            || radius_level_zero_pixels > Self::MAX_RADIUS_LEVEL_ZERO_PIXELS
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
        })
    }

    pub fn with_behavior(
        mut self,
        mode: RetouchMode,
        source_offset_x_radii: f64,
        source_offset_y_radii: f64,
        feather: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let source_offset_x_radii = FiniteF64::new(source_offset_x_radii)?;
        let source_offset_y_radii = FiniteF64::new(source_offset_y_radii)?;
        if !(-2.0..=2.0).contains(&source_offset_x_radii.get())
            || !(-2.0..=2.0).contains(&source_offset_y_radii.get())
        {
            return Err(RecipeValidationError::InvalidRetouchSourceOffset);
        }
        self.mode = mode;
        self.source_offset_x_radii = source_offset_x_radii;
        self.source_offset_y_radii = source_offset_y_radii;
        self.feather = feather;
        Ok(self)
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

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(self.points.clone(), self.radius_level_zero_pixels)?
            .with_behavior(
                self.mode,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                self.feather,
            )
            .map(|_| ())
    }
}

/// A recipe limits the number of independently editable repair strokes just
/// as it limits legacy circular repair spots.
pub const MAX_RETOUCH_STROKES_PER_RECIPE: usize = 64;
/// One drag may retain at most this many normalized centerline samples.
pub const MAX_RETOUCH_STROKE_POINTS: usize = 512;

//! Photo-local, non-generative repair and clone recipe contracts.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;
use super::value::{FiniteF64, UnitInterval, default_finite_zero};

const MAX_RETOUCH_DETAIL_APRON_LEVEL_ZERO_PIXELS: f64 = 512.0;

fn maximum_source_offset_radii(radius_level_zero_pixels: u16) -> f64 {
    (MAX_RETOUCH_DETAIL_APRON_LEVEL_ZERO_PIXELS - 1.0) / f64::from(radius_level_zero_pixels) - 1.0
}

fn valid_source_offsets(
    radius_level_zero_pixels: u16,
    horizontal_source_offset_radii: f64,
    vertical_source_offset_radii: f64,
    source_scale: f64,
) -> bool {
    let maximum = maximum_source_offset_radii(radius_level_zero_pixels) + 1.0 - source_scale;
    horizontal_source_offset_radii.abs() <= maximum && vertical_source_offset_radii.abs() <= maximum
}

const fn default_retouch_source_scale() -> FiniteF64 {
    FiniteF64(1.0)
}

fn valid_source_transform(rotation_degrees: f64, scale: f64) -> bool {
    (-180.0..=180.0).contains(&rotation_degrees) && (0.25..=4.0).contains(&scale)
}

/// Deterministic non-generative repair behavior.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RetouchMode {
    /// Preserve nearby donor texture while matching the target boundary.
    #[default]
    Heal,
    /// Copy from a nearby source offset measured in brush radii.
    Clone,
    /// Preserve the stronger target or donor gradient while matching the
    /// target boundary. This is intended for repairs that cross a real edge;
    /// ordinary Heal remains the safer choice for isolated dust and spots.
    HealStructure,
    /// Copy only the low-frequency color and illumination component.
    Tone,
    /// Copy only the signed high-frequency texture component.
    Texture,
}

const fn default_frequency_radius() -> u16 {
    8
}
const fn is_default_frequency_radius(value: &u16) -> bool {
    *value == 8
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
/// radii and bounded by the 512-pixel full-detail apron, preserving tile-local
/// execution while allowing a small brush to use a substantially farther
/// source than the former fixed eight-radius limit.
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
    #[serde(default = "default_finite_zero")]
    source_rotation_degrees: FiniteF64,
    #[serde(default = "default_retouch_source_scale")]
    source_scale: FiniteF64,
    #[serde(default)]
    source_flip_horizontal: bool,
    #[serde(default)]
    source_flip_vertical: bool,
    #[serde(default = "default_retouch_feather")]
    feather: UnitInterval,
    #[serde(default = "default_retouch_strength")]
    strength: UnitInterval,
    #[serde(
        default = "default_frequency_radius",
        skip_serializing_if = "is_default_frequency_radius"
    )]
    frequency_radius: u16,
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
            source_rotation_degrees: default_finite_zero(),
            source_scale: default_retouch_source_scale(),
            source_flip_horizontal: false,
            source_flip_vertical: false,
            feather: default_retouch_feather(),
            strength: default_retouch_strength(),
            frequency_radius: default_frequency_radius(),
        })
    }

    /// Applies the repair algorithm, source displacement, and edge feather.
    ///
    /// # Errors
    ///
    /// Returns an error when either source displacement is non-finite or its
    /// radius-scaled donor footprint exceeds the full-detail apron.
    pub fn with_behavior(
        mut self,
        mode: RetouchMode,
        horizontal_source_offset_radii: f64,
        vertical_source_offset_radii: f64,
        feather: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let horizontal_offset = FiniteF64::new(horizontal_source_offset_radii)?;
        let vertical_offset = FiniteF64::new(vertical_source_offset_radii)?;
        if !valid_source_offsets(
            self.radius_level_zero_pixels,
            horizontal_offset.get(),
            vertical_offset.get(),
            self.source_scale.get(),
        ) {
            return Err(RecipeValidationError::InvalidRetouchSourceOffset);
        }
        self.mode = mode;
        self.source_offset_x_radii = horizontal_offset;
        self.source_offset_y_radii = vertical_offset;
        self.feather = feather;
        Ok(self)
    }

    /// Rotates, scales, or mirrors donor coordinates around the target anchor.
    /// Identity values preserve the historical translated-source behavior.
    ///
    /// # Errors
    ///
    /// Returns an error when the rotation, scale, or resulting donor footprint
    /// is outside the supported retouch bounds.
    pub fn with_source_transform(
        mut self,
        rotation_degrees: f64,
        scale: f64,
        flip_horizontal: bool,
        flip_vertical: bool,
    ) -> Result<Self, RecipeValidationError> {
        let rotation = FiniteF64::new(rotation_degrees)?;
        let scale = FiniteF64::new(scale)?;
        if !valid_source_transform(rotation.get(), scale.get())
            || !valid_source_offsets(
                self.radius_level_zero_pixels,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                scale.get(),
            )
        {
            return Err(RecipeValidationError::InvalidRetouchSourceTransform);
        }
        self.source_rotation_degrees = rotation;
        self.source_scale = scale;
        self.source_flip_horizontal = flip_horizontal;
        self.source_flip_vertical = flip_vertical;
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

    pub const fn source_rotation_degrees(self) -> f64 {
        self.source_rotation_degrees.get()
    }

    pub const fn source_scale(self) -> f64 {
        self.source_scale.get()
    }

    pub const fn source_flip_horizontal(self) -> bool {
        self.source_flip_horizontal
    }

    pub const fn source_flip_vertical(self) -> bool {
        self.source_flip_vertical
    }

    pub const fn feather(self) -> UnitInterval {
        self.feather
    }

    pub const fn strength(self) -> UnitInterval {
        self.strength
    }

    /// Gaussian separation scale in original-image pixels, independent of brush size.
    pub fn with_frequency_radius(mut self, radius: u16) -> Result<Self, RecipeValidationError> {
        if !(2..=32).contains(&radius) {
            return Err(RecipeValidationError::InvalidRetouchFrequencyRadius(radius));
        }
        self.frequency_radius = radius;
        Ok(self)
    }

    pub const fn frequency_radius(&self) -> u16 {
        self.frequency_radius
    }

    pub(super) fn validate(self) -> Result<(), RecipeValidationError> {
        Self::new(self.center_x, self.center_y, self.radius_level_zero_pixels)?
            .with_source_transform(
                self.source_rotation_degrees.get(),
                self.source_scale.get(),
                self.source_flip_horizontal,
                self.source_flip_vertical,
            )?
            .with_behavior(
                self.mode,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                self.feather,
            )
            .and_then(|value| value.with_frequency_radius(self.frequency_radius))
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
    #[serde(default = "default_finite_zero")]
    source_rotation_degrees: FiniteF64,
    #[serde(default = "default_retouch_source_scale")]
    source_scale: FiniteF64,
    #[serde(default)]
    source_flip_horizontal: bool,
    #[serde(default)]
    source_flip_vertical: bool,
    #[serde(default = "default_retouch_feather")]
    feather: UnitInterval,
    #[serde(default = "default_retouch_strength")]
    strength: UnitInterval,
    #[serde(
        default = "default_frequency_radius",
        skip_serializing_if = "is_default_frequency_radius"
    )]
    frequency_radius: u16,
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
            source_rotation_degrees: default_finite_zero(),
            source_scale: default_retouch_source_scale(),
            source_flip_horizontal: false,
            source_flip_vertical: false,
            feather: default_retouch_feather(),
            strength: default_retouch_strength(),
            frequency_radius: default_frequency_radius(),
        })
    }

    /// Applies the repair algorithm, shared source displacement, and edge
    /// feather to the complete continuous stroke.
    ///
    /// # Errors
    ///
    /// Returns an error when either source displacement is non-finite or its
    /// radius-scaled donor footprint exceeds the full-detail apron.
    pub fn with_behavior(
        mut self,
        mode: RetouchMode,
        horizontal_source_offset_radii: f64,
        vertical_source_offset_radii: f64,
        feather: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let horizontal_offset = FiniteF64::new(horizontal_source_offset_radii)?;
        let vertical_offset = FiniteF64::new(vertical_source_offset_radii)?;
        if !valid_source_offsets(
            self.radius_level_zero_pixels,
            horizontal_offset.get(),
            vertical_offset.get(),
            self.source_scale.get(),
        ) {
            return Err(RecipeValidationError::InvalidRetouchSourceOffset);
        }
        self.mode = mode;
        self.source_offset_x_radii = horizontal_offset;
        self.source_offset_y_radii = vertical_offset;
        self.feather = feather;
        Ok(self)
    }

    /// Applies a bounded donor-coordinate transform around the stroke's
    /// normalized bounds center.
    ///
    /// # Errors
    ///
    /// Returns an error when the rotation, scale, or resulting donor footprint
    /// is outside the supported retouch bounds.
    pub fn with_source_transform(
        mut self,
        rotation_degrees: f64,
        scale: f64,
        flip_horizontal: bool,
        flip_vertical: bool,
    ) -> Result<Self, RecipeValidationError> {
        let rotation = FiniteF64::new(rotation_degrees)?;
        let scale = FiniteF64::new(scale)?;
        if !valid_source_transform(rotation.get(), scale.get())
            || !valid_source_offsets(
                self.radius_level_zero_pixels,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                scale.get(),
            )
        {
            return Err(RecipeValidationError::InvalidRetouchSourceTransform);
        }
        self.source_rotation_degrees = rotation;
        self.source_scale = scale;
        self.source_flip_horizontal = flip_horizontal;
        self.source_flip_vertical = flip_vertical;
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

    pub const fn source_rotation_degrees(&self) -> f64 {
        self.source_rotation_degrees.get()
    }

    pub const fn source_scale(&self) -> f64 {
        self.source_scale.get()
    }

    pub const fn source_flip_horizontal(&self) -> bool {
        self.source_flip_horizontal
    }

    pub const fn source_flip_vertical(&self) -> bool {
        self.source_flip_vertical
    }

    pub const fn feather(&self) -> UnitInterval {
        self.feather
    }

    pub const fn strength(&self) -> UnitInterval {
        self.strength
    }

    /// Gaussian separation scale in original-image pixels, independent of brush size.
    pub fn with_frequency_radius(mut self, radius: u16) -> Result<Self, RecipeValidationError> {
        if !(2..=32).contains(&radius) {
            return Err(RecipeValidationError::InvalidRetouchFrequencyRadius(radius));
        }
        self.frequency_radius = radius;
        Ok(self)
    }

    pub const fn frequency_radius(&self) -> u16 {
        self.frequency_radius
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(self.points.clone(), self.radius_level_zero_pixels)?
            .with_source_transform(
                self.source_rotation_degrees.get(),
                self.source_scale.get(),
                self.source_flip_horizontal,
                self.source_flip_vertical,
            )?
            .with_behavior(
                self.mode,
                self.source_offset_x_radii.get(),
                self.source_offset_y_radii.get(),
                self.feather,
            )
            .and_then(|value| value.with_frequency_radius(self.frequency_radius))
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

//! Recipe-local spatial-mask definitions and immutable revisions.

mod managed_raster;

use serde::{Deserialize, Serialize};

use crate::MaskId;

pub use managed_raster::{
    MANAGED_RASTER_MASK_REFERENCE_VERSION, MAX_MANAGED_RASTER_MASK_DIMENSION, ManagedRasterMask,
    RasterMaskEncoding,
};

use super::{
    FiniteF64, MaskCoordinateSpace, RecipeValidationError, UnitInterval,
    condition_mask::{ConditionMaskExpression, LegacyConditionLeaf},
};

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct MaskReference {
    mask_id: MaskId,
    revision: u32,
    coordinate_space: MaskCoordinateSpace,
}

impl MaskReference {
    /// Creates a reference to one immutable mask revision.
    ///
    /// # Errors
    ///
    /// Returns an error when `revision` is zero.
    pub fn new(
        mask_id: MaskId,
        revision: u32,
        coordinate_space: MaskCoordinateSpace,
    ) -> Result<Self, RecipeValidationError> {
        if revision == 0 {
            return Err(RecipeValidationError::ZeroMaskRevision);
        }
        Ok(Self {
            mask_id,
            revision,
            coordinate_space,
        })
    }

    pub const fn mask_id(self) -> MaskId {
        self.mask_id
    }

    pub const fn revision(self) -> u32 {
        self.revision
    }

    pub const fn coordinate_space(self) -> MaskCoordinateSpace {
        self.coordinate_space
    }
}

/// One renderer-neutral spatial or pixel-condition mask.
///
/// Local masks deliberately describe *where* a layer applies, never which
/// adjustment it contains. A Grade Node can therefore remain a complete,
/// reusable adjustment while each photo instance supplies its own placement.
/// Geometry uses normalized image coordinates; condition masks describe
/// normalized perceptual ranges evaluated against that layer's input pixels.
/// Both share one ownership/revision contract, so a future AI-generated mask
/// can be added without changing Grade Node identity.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MaskDefinition {
    /// A smooth 0-to-1 ramp from `start` to `end`. Pixels before `start` have
    /// zero coverage; pixels after `end` have full coverage.
    LinearGradient {
        start_x: UnitInterval,
        start_y: UnitInterval,
        end_x: UnitInterval,
        end_y: UnitInterval,
        #[serde(default)]
        invert: bool,
    },
    /// An elliptical mask centered at `center`. Coverage is full inside the
    /// inner ellipse and falls to zero across `feather` of its radius.
    RadialGradient {
        center_x: UnitInterval,
        center_y: UnitInterval,
        radius_x: UnitInterval,
        radius_y: UnitInterval,
        feather: UnitInterval,
        #[serde(default)]
        invert: bool,
    },
    /// One or more freehand strokes. A point marked `begins_stroke` starts a
    /// disconnected stroke; subsequent points are joined by round segments.
    Brush {
        points: Vec<MaskBrushPoint>,
        radius: UnitInterval,
        feather: UnitInterval,
        #[serde(default)]
        invert: bool,
    },
    /// A perceptual Oklab-lightness interval. `lower` and `upper` bound the
    /// full-coverage region; `softness` describes the normalized transition
    /// outside those boundaries.
    LuminanceRange {
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
        #[serde(default)]
        invert: bool,
    },
    /// A circular Oklch-hue interval. `width_degrees` is the half-width around
    /// a canonical hue in `[0, 360)`.
    ColorRange {
        center_hue_degrees: FiniteF64,
        width_degrees: FiniteF64,
        softness: UnitInterval,
        #[serde(default)]
        invert: bool,
    },
    /// A bounded typed condition that cannot be represented by the legacy
    /// single luminance or hue leaf. Runtime support is capability-gated at
    /// the Recipe compiler; persistence never implies executability.
    ConditionExpression { expression: ConditionMaskExpression },
    /// Immutable application-managed grayscale coverage. The reference carries
    /// exact byte identity and both stored/output coordinate extents; provider
    /// caches and model installation are not part of Recipe execution.
    ///
    /// Refinement is stored as exact integer percentages so UI gestures cannot
    /// accumulate floating-point drift. Positive expansion grows coverage,
    /// negative expansion contracts it, and feather adds a symmetric soft
    /// transition. Runtime maps 100% to a bounded fraction of the stored
    /// raster's shorter edge.
    ManagedRaster {
        raster: ManagedRasterMask,
        #[serde(default)]
        expansion_percent: i8,
        #[serde(default)]
        feather_percent: u8,
        #[serde(default)]
        invert: bool,
    },
}

/// One immutable freehand-mask sample in original-image coordinates.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct MaskBrushPoint {
    x: UnitInterval,
    y: UnitInterval,
    begins_stroke: bool,
}

impl MaskBrushPoint {
    pub const fn new(x: UnitInterval, y: UnitInterval, begins_stroke: bool) -> Self {
        Self {
            x,
            y,
            begins_stroke,
        }
    }

    pub const fn x(self) -> UnitInterval {
        self.x
    }

    pub const fn y(self) -> UnitInterval {
        self.y
    }

    pub const fn begins_stroke(self) -> bool {
        self.begins_stroke
    }
}

pub const MAX_MASK_BRUSH_POINTS: usize = 4_096;

impl MaskDefinition {
    /// Creates a normalized linear-gradient mask after validating that it has
    /// a measurable direction.
    ///
    /// # Errors
    ///
    /// Returns an error when the start and end coordinates do not define a
    /// measurable direction.
    pub fn linear_gradient(
        start_x: UnitInterval,
        start_y: UnitInterval,
        end_x: UnitInterval,
        end_y: UnitInterval,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let definition = Self::LinearGradient {
            start_x,
            start_y,
            end_x,
            end_y,
            invert,
        };
        definition.validate()?;
        Ok(definition)
    }

    /// Creates a normalized radial-gradient mask after validating its
    /// non-zero ellipse radii.
    ///
    /// # Errors
    ///
    /// Returns an error when either normalized ellipse radius is zero.
    pub fn radial_gradient(
        center_x: UnitInterval,
        center_y: UnitInterval,
        radius_x: UnitInterval,
        radius_y: UnitInterval,
        feather: UnitInterval,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let definition = Self::RadialGradient {
            center_x,
            center_y,
            radius_x,
            radius_y,
            feather,
            invert,
        };
        definition.validate()?;
        Ok(definition)
    }

    /// Creates an editable freehand mask. No points means zero coverage until
    /// the first stroke is drawn, which is valid persistent tool state.
    ///
    /// # Errors
    ///
    /// Returns an error when the brush contains more than
    /// [`MAX_MASK_BRUSH_POINTS`] samples.
    pub fn brush(
        points: Vec<MaskBrushPoint>,
        radius: UnitInterval,
        feather: UnitInterval,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let definition = Self::Brush {
            points,
            radius,
            feather,
            invert,
        };
        definition.validate()?;
        Ok(definition)
    }

    /// Creates an Oklab-lightness range mask.
    ///
    /// # Errors
    ///
    /// Returns an error when `lower` exceeds `upper`.
    pub fn luminance_range(
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let definition = Self::LuminanceRange {
            lower,
            upper,
            softness,
            invert,
        };
        definition.validate()?;
        Ok(definition)
    }

    /// Creates a circular Oklch-hue range mask.
    ///
    /// Any finite hue is normalized to the canonical interval `[0, 360)`.
    ///
    /// # Errors
    ///
    /// Returns an error when either degree value is non-finite or when the
    /// half-width lies outside `1°..=180°`.
    pub fn color_range(
        center_hue_degrees: f64,
        width_degrees: f64,
        softness: UnitInterval,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let center_hue_degrees = FiniteF64::new(center_hue_degrees)?;
        let normalized_hue = center_hue_degrees.get().rem_euclid(360.0);
        // Collapse negative zero so semantically identical hues serialize and
        // hash to exactly one persistent representation.
        let normalized_hue = if normalized_hue == 0.0 {
            0.0
        } else {
            normalized_hue
        };
        let definition = Self::ColorRange {
            center_hue_degrees: FiniteF64::new(normalized_hue)?,
            width_degrees: FiniteF64::new(width_degrees)?,
            softness,
            invert,
        };
        definition.validate()?;
        Ok(definition)
    }

    /// Stores a bounded condition expression in its canonical Recipe shape.
    ///
    /// A single luminance leaf or zero-minimum-chroma hue leaf is rewritten to
    /// the existing v1 mask variant. This preserves the byte representation
    /// and content-derived identity used before composite conditions existed.
    ///
    /// # Errors
    ///
    /// Returns an error when the expression violates its fixed schema or tree
    /// bounds.
    pub fn condition_expression(
        expression: ConditionMaskExpression,
    ) -> Result<Self, RecipeValidationError> {
        expression.validate()?;
        match expression.legacy_leaf() {
            Some(LegacyConditionLeaf::Luminance {
                lower,
                upper,
                softness,
                invert,
            }) => Self::luminance_range(lower, upper, softness, invert),
            Some(LegacyConditionLeaf::Hue {
                center_hue_degrees,
                half_width_degrees,
                softness,
                invert,
            }) => Self::color_range(
                center_hue_degrees.get(),
                half_width_degrees.get(),
                softness,
                invert,
            ),
            None => {
                let definition = Self::ConditionExpression { expression };
                definition.validate()?;
                Ok(definition)
            }
        }
    }

    /// Stores an accepted managed soft-mask raster as immutable Recipe state.
    ///
    /// # Errors
    ///
    /// Returns an error when the managed content identity, storage identity,
    /// extent, or tightly packed byte shape is inconsistent.
    pub fn managed_raster(
        raster: ManagedRasterMask,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        Self::managed_raster_with_refinement(raster, 0, 0, invert)
    }

    /// Stores an accepted managed soft-mask raster with reversible edge
    /// refinement.
    ///
    /// `expansion_percent` is in `[-100, 100]`; `feather_percent` is in
    /// `[0, 100]`. The values are renderer-neutral authoring controls, not
    /// provider settings, so changing them never reruns or mutates the model
    /// output.
    ///
    /// # Errors
    ///
    /// Returns an error when the managed raster reference is inconsistent or
    /// either refinement percentage is outside its fixed range.
    pub fn managed_raster_with_refinement(
        raster: ManagedRasterMask,
        expansion_percent: i8,
        feather_percent: u8,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let definition = Self::ManagedRaster {
            raster,
            expansion_percent,
            feather_percent,
            invert,
        };
        definition.validate()?;
        Ok(definition)
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        match self {
            Self::LinearGradient {
                start_x,
                start_y,
                end_x,
                end_y,
                ..
            } => {
                let dx = end_x.get() - start_x.get();
                let dy = end_y.get() - start_y.get();
                if dx.mul_add(dx, dy * dy) <= f64::EPSILON {
                    return Err(RecipeValidationError::DegenerateLinearMask);
                }
            }
            Self::RadialGradient {
                radius_x, radius_y, ..
            } => {
                if radius_x.get() <= 0.0 || radius_y.get() <= 0.0 {
                    return Err(RecipeValidationError::DegenerateRadialMask);
                }
            }
            Self::Brush { points, radius, .. } => {
                if points.len() > MAX_MASK_BRUSH_POINTS {
                    return Err(RecipeValidationError::TooManyMaskBrushPoints(points.len()));
                }
                if radius.get() <= 0.0 {
                    return Err(RecipeValidationError::DegenerateBrushMask);
                }
            }
            Self::LuminanceRange { lower, upper, .. } => {
                if lower > upper {
                    return Err(RecipeValidationError::InvalidLuminanceMaskRange {
                        lower: lower.get(),
                        upper: upper.get(),
                    });
                }
            }
            Self::ColorRange {
                center_hue_degrees,
                width_degrees,
                ..
            } => {
                if !(0.0..360.0).contains(&center_hue_degrees.get()) {
                    return Err(RecipeValidationError::InvalidColorMaskHue(
                        center_hue_degrees.get(),
                    ));
                }
                if !(1.0..=180.0).contains(&width_degrees.get()) {
                    return Err(RecipeValidationError::InvalidColorMaskWidth(
                        width_degrees.get(),
                    ));
                }
            }
            Self::ConditionExpression { expression } => {
                expression.validate()?;
                if expression.legacy_leaf().is_some() {
                    return Err(RecipeValidationError::NonCanonicalConditionMaskExpression);
                }
            }
            Self::ManagedRaster {
                raster,
                expansion_percent,
                feather_percent,
                ..
            } => {
                raster.validate()?;
                if !(-100..=100).contains(expansion_percent) {
                    return Err(RecipeValidationError::InvalidManagedRasterMaskExpansion(
                        *expansion_percent,
                    ));
                }
                if *feather_percent > 100 {
                    return Err(RecipeValidationError::InvalidManagedRasterMaskFeather(
                        *feather_percent,
                    ));
                }
            }
        }
        Ok(())
    }
}

/// One immutable, recipe-local revision of a spatial mask.
///
/// Recipes carry the definition rather than merely an identifier so a saved
/// version can still reproduce its pixels after a user revises or deletes a
/// similarly named mask in a later workspace state.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MaskRevision {
    id: MaskId,
    revision: u32,
    coordinate_space: MaskCoordinateSpace,
    definition: MaskDefinition,
}

impl MaskRevision {
    /// Creates one immutable mask revision.
    ///
    /// # Errors
    ///
    /// Returns an error for a zero revision or a degenerate shape.
    pub fn new(
        id: MaskId,
        revision: u32,
        coordinate_space: MaskCoordinateSpace,
        definition: MaskDefinition,
    ) -> Result<Self, RecipeValidationError> {
        if revision == 0 {
            return Err(RecipeValidationError::ZeroMaskRevision);
        }
        definition.validate()?;
        Ok(Self {
            id,
            revision,
            coordinate_space,
            definition,
        })
    }

    pub const fn id(&self) -> MaskId {
        self.id
    }

    pub const fn revision(&self) -> u32 {
        self.revision
    }

    pub const fn coordinate_space(&self) -> MaskCoordinateSpace {
        self.coordinate_space
    }

    pub const fn definition(&self) -> &MaskDefinition {
        &self.definition
    }

    pub fn reference(&self) -> MaskReference {
        // Construction has already checked this immutable revision.
        MaskReference {
            mask_id: self.id,
            revision: self.revision,
            coordinate_space: self.coordinate_space,
        }
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(
            self.id,
            self.revision,
            self.coordinate_space,
            self.definition.clone(),
        )
        .map(|_| ())
    }
}

#[cfg(test)]
mod tests;

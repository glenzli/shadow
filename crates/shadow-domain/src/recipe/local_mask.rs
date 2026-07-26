//! Recipe-local spatial-mask definitions and immutable revisions.

use serde::{Deserialize, Serialize};

use crate::MaskId;

use super::{MaskCoordinateSpace, RecipeValidationError, UnitInterval};

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

/// One renderer-neutral spatial mask shape stored in normalized image
/// coordinates.
///
/// Local masks deliberately describe *where* a layer applies, never which
/// adjustment it contains. A Grade Node can therefore remain a complete,
/// reusable adjustment while each photo instance supplies its own placement.
/// Gradient and freehand shapes share one ownership/revision contract, so a
/// future AI-generated mask can be added without changing Grade Node identity.
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

    fn validate(&self) -> Result<(), RecipeValidationError> {
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

//! Photo-local, non-shareable liquify authoring contracts.
//!
//! This module stores user intent, not a renderer-generated displacement
//! texture or preview mesh. Renderers deterministically resample these paths
//! into resolution-specific brush stamps and an inverse displacement field.

use serde::{Deserialize, Serialize};

use super::{RecipeValidationError, UnitInterval};

/// Maximum authored gestures retained by the singleton liquify node.
pub const MAX_LIQUIFY_STROKES_PER_NODE: usize = 128;

/// Maximum pointer samples retained by one authored liquify gesture.
pub const MAX_LIQUIFY_POINTS_PER_STROKE: usize = 2_048;

/// One pressure-bearing sample in the uncropped original-image coordinate space.
///
/// `x` and `y` use normalized image-edge coordinates. They therefore do not
/// change when a later crop is reset or adjusted. Pressure is normalized to
/// `[0, 1]`; mouse input normally authors `1`.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct LiquifyPoint {
    x: UnitInterval,
    y: UnitInterval,
    pressure: UnitInterval,
}

impl LiquifyPoint {
    /// Creates one full-pressure pointer sample.
    pub const fn new(x: UnitInterval, y: UnitInterval) -> Self {
        Self {
            x,
            y,
            pressure: UnitInterval::ONE,
        }
    }

    /// Creates one pointer sample with explicit normalized pressure.
    pub const fn with_pressure(x: UnitInterval, y: UnitInterval, pressure: UnitInterval) -> Self {
        Self { x, y, pressure }
    }

    pub const fn x(self) -> UnitInterval {
        self.x
    }

    pub const fn y(self) -> UnitInterval {
        self.y
    }

    pub const fn pressure(self) -> UnitInterval {
        self.pressure
    }
}

/// One durable liquify gesture.
///
/// The tagged sum type reserves a stable persistence boundary for later
/// twirl, pucker, and bloat gestures without pretending those tools share the
/// same authoring parameters. The first implementation is the common push
/// brush: each path segment moves content from its first sample toward its
/// second sample.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum LiquifyStroke {
    Push {
        points: Vec<LiquifyPoint>,
        /// Brush radius as a fraction of the original image's shorter edge.
        radius: UnitInterval,
        /// Normalized displacement gain applied to every path segment.
        strength: UnitInterval,
        /// Normalized inner falloff; `1` keeps a harder core.
        hardness: UnitInterval,
    },
}

impl LiquifyStroke {
    /// Creates one validated push-brush gesture.
    ///
    /// # Errors
    ///
    /// Returns an error when the path has fewer than two or too many samples,
    /// or when radius/strength would make the gesture a non-canonical no-op.
    pub fn push(
        points: Vec<LiquifyPoint>,
        radius: UnitInterval,
        strength: UnitInterval,
        hardness: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let stroke = Self::Push {
            points,
            radius,
            strength,
            hardness,
        };
        stroke.validate()?;
        Ok(stroke)
    }

    /// Returns the authored samples in input order.
    pub fn points(&self) -> &[LiquifyPoint] {
        match self {
            Self::Push { points, .. } => points,
        }
    }

    pub const fn radius(&self) -> UnitInterval {
        match self {
            Self::Push { radius, .. } => *radius,
        }
    }

    pub const fn strength(&self) -> UnitInterval {
        match self {
            Self::Push { strength, .. } => *strength,
        }
    }

    pub const fn hardness(&self) -> UnitInterval {
        match self {
            Self::Push { hardness, .. } => *hardness,
        }
    }

    fn validate(&self) -> Result<(), RecipeValidationError> {
        let points = self.points();
        if points.len() < 2 {
            return Err(RecipeValidationError::TooFewLiquifyStrokePoints(
                points.len(),
            ));
        }
        if points.len() > MAX_LIQUIFY_POINTS_PER_STROKE {
            return Err(RecipeValidationError::TooManyLiquifyStrokePoints(
                points.len(),
            ));
        }
        let has_effective_motion = points.windows(2).any(|segment| {
            let from = segment[0];
            let to = segment[1];
            (from.x() != to.x() || from.y() != to.y())
                && (from.pressure() != UnitInterval::ZERO || to.pressure() != UnitInterval::ZERO)
        });
        if !has_effective_motion {
            return Err(RecipeValidationError::DegenerateLiquifyStroke);
        }
        if self.radius().get() == 0.0 {
            return Err(RecipeValidationError::DegenerateLiquifyBrushRadius);
        }
        if self.strength().get() == 0.0 {
            return Err(RecipeValidationError::DegenerateLiquifyStrength);
        }
        Ok(())
    }
}

/// The optional, singleton liquify node for one photograph.
///
/// A persisted node is always non-empty. An editor may show a transient empty
/// tool session before the first gesture, but committing that state canonicalizes
/// to `None` in [`super::PhotoStructuralNodes`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PhotoLiquifyNode {
    strokes: Vec<LiquifyStroke>,
}

impl PhotoLiquifyNode {
    /// Creates one validated photo-local liquify node.
    ///
    /// # Errors
    ///
    /// Returns an error when the node is empty, exceeds its fixed gesture
    /// bound, or contains an invalid gesture.
    pub fn new(strokes: Vec<LiquifyStroke>) -> Result<Self, RecipeValidationError> {
        let node = Self { strokes };
        node.validate()?;
        Ok(node)
    }

    /// Returns authored gestures in their deterministic composition order.
    pub fn strokes(&self) -> &[LiquifyStroke] {
        &self.strokes
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.strokes.is_empty() {
            return Err(RecipeValidationError::EmptyLiquifyNode);
        }
        if self.strokes.len() > MAX_LIQUIFY_STROKES_PER_NODE {
            return Err(RecipeValidationError::TooManyLiquifyStrokes(
                self.strokes.len(),
            ));
        }
        for stroke in &self.strokes {
            stroke.validate()?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;

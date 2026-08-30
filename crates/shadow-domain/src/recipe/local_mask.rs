//! Recipe-local spatial-mask definitions and immutable revisions.

mod managed_raster;
mod semantic;

use std::collections::HashSet;

use serde::{Deserialize, Deserializer, Serialize, de::Error as _};

use crate::{MaskComponentId, MaskId};

pub use managed_raster::{
    MANAGED_RASTER_MASK_REFERENCE_VERSION, MAX_MANAGED_RASTER_MASK_DIMENSION, ManagedRasterMask,
    RasterMaskEncoding,
};
pub use semantic::{
    MAX_SEMANTIC_MASK_QUERY_BYTES, MAX_SEMANTIC_MASK_REGIONS,
    SEMANTIC_MASK_INTENT_CONTRACT_VERSION, SemanticMaskAggregation, SemanticMaskIntent,
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

/// Maximum number of ordered mask components in one immutable mask revision.
pub const MAX_MASK_COMPONENTS: usize = 8;

/// Ordered soft-coverage operation for one component of a composite mask.
///
/// Operations use bounded min/max set algebra so binary masks retain ordinary
/// Boolean behavior while feathered authoring remains continuous and
/// idempotent: Base returns the new coverage, Add takes the larger coverage,
/// Subtract caps coverage by the complement, and Intersect takes the smaller
/// coverage.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MaskComponentOperation {
    Base,
    Add,
    Subtract,
    Intersect,
}

impl MaskComponentOperation {
    /// Combines one scalar soft-coverage sample with the accumulated coverage.
    ///
    /// This is the renderer-neutral reference operation used to keep CPU,
    /// accelerated preview, detail, and export implementations equivalent.
    pub fn combine_coverage(
        self,
        accumulated: UnitInterval,
        component: UnitInterval,
    ) -> UnitInterval {
        let accumulated = accumulated.get();
        let component = component.get();
        let coverage = match self {
            Self::Base => component,
            Self::Add => accumulated.max(component),
            Self::Subtract => accumulated.min(1.0 - component),
            Self::Intersect => accumulated.min(component),
        };
        UnitInterval(FiniteF64(coverage.clamp(0.0, 1.0)))
    }
}

/// One stable, independently bypassable leaf in a composite mask.
///
/// The definition is always an existing non-composite mask definition. This
/// makes one MaskRevision the sole persistence owner while preventing an
/// unbounded second expression tree.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MaskComponent {
    id: MaskComponentId,
    operation: MaskComponentOperation,
    #[serde(default = "mask_component_enabled_default")]
    enabled: bool,
    definition: MaskDefinition,
}

impl MaskComponent {
    /// Creates one validated non-nested component.
    ///
    /// Position-specific operation rules are validated by MaskComposite::new.
    ///
    /// # Errors
    ///
    /// Returns an error for a nested composite or invalid leaf definition.
    pub fn new(
        id: MaskComponentId,
        operation: MaskComponentOperation,
        enabled: bool,
        definition: MaskDefinition,
    ) -> Result<Self, RecipeValidationError> {
        let component = Self {
            id,
            operation,
            enabled,
            definition,
        };
        component.validate()?;
        Ok(component)
    }

    pub const fn id(&self) -> MaskComponentId {
        self.id
    }

    pub const fn operation(&self) -> MaskComponentOperation {
        self.operation
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    pub const fn definition(&self) -> &MaskDefinition {
        &self.definition
    }

    /// Preserves the component identity and authored leaf while changing only
    /// its local bypass state.
    #[must_use]
    pub const fn with_enabled(mut self, enabled: bool) -> Self {
        self.enabled = enabled;
        self
    }

    fn validate(&self) -> Result<(), RecipeValidationError> {
        if matches!(self.definition, MaskDefinition::Composite { .. }) {
            return Err(RecipeValidationError::NestedMaskComposite);
        }
        self.definition.validate()
    }
}

/// One bounded, ordered composition owned by an existing mask revision.
///
/// Component order is authored state. The first component establishes the
/// base; every later component modifies the accumulated soft coverage.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct MaskComposite {
    components: Vec<MaskComponent>,
    #[serde(default)]
    invert: bool,
}

#[derive(Deserialize)]
struct UnvalidatedMaskComposite {
    components: Vec<MaskComponent>,
    #[serde(default)]
    invert: bool,
}

impl TryFrom<UnvalidatedMaskComposite> for MaskComposite {
    type Error = RecipeValidationError;

    fn try_from(value: UnvalidatedMaskComposite) -> Result<Self, Self::Error> {
        Self::new(value.components, value.invert)
    }
}

impl<'de> Deserialize<'de> for MaskComposite {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        UnvalidatedMaskComposite::deserialize(deserializer)?
            .try_into()
            .map_err(D::Error::custom)
    }
}

impl MaskComposite {
    /// Creates one validated, ordered composition.
    ///
    /// A one-component enabled Base with no final inversion is valid here so
    /// editing code can remove a component atomically. Use
    /// MaskDefinition::composite to normalize that reducible case back to the
    /// legacy single-mask wire shape.
    ///
    /// # Errors
    ///
    /// Returns an error for an empty or oversized composition, duplicate
    /// identity, nested composition, or invalid operation order.
    pub fn new(
        components: Vec<MaskComponent>,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let composite = Self { components, invert };
        composite.validate()?;
        Ok(composite)
    }

    pub fn components(&self) -> &[MaskComponent] {
        &self.components
    }

    pub const fn invert(&self) -> bool {
        self.invert
    }

    /// Reports whether this composition carries no authored state beyond one
    /// legacy leaf definition.
    pub fn is_reducible_to_single_definition(&self) -> bool {
        self.components.len() == 1
            && self.components[0].operation == MaskComponentOperation::Base
            && self.components[0].enabled
            && !self.invert
    }

    /// Evaluates one scalar coverage sample in stable component order.
    ///
    /// component_coverages includes one entry for every component, including
    /// bypassed entries. Disabled components are skipped without changing
    /// alignment, and final inversion is applied after all enabled operations.
    ///
    /// # Errors
    ///
    /// Returns an error when coverage arity differs from the persisted
    /// component count.
    pub fn reference_coverage(
        &self,
        component_coverages: &[UnitInterval],
    ) -> Result<UnitInterval, RecipeValidationError> {
        if component_coverages.len() != self.components.len() {
            return Err(RecipeValidationError::MaskComponentCoverageArity {
                expected: self.components.len(),
                actual: component_coverages.len(),
            });
        }
        let coverage = self
            .components
            .iter()
            .zip(component_coverages)
            .filter(|(component, _)| component.enabled)
            .fold(UnitInterval::ZERO, |coverage, (component, next)| {
                component.operation.combine_coverage(coverage, *next)
            });
        if self.invert {
            Ok(UnitInterval(FiniteF64(1.0 - coverage.get())))
        } else {
            Ok(coverage)
        }
    }

    /// Returns the legacy leaf representation when no composite-only authored
    /// state would be lost.
    pub fn into_single_definition(mut self) -> Result<MaskDefinition, Self> {
        if self.is_reducible_to_single_definition() {
            if let Some(component) = self.components.pop() {
                return Ok(component.definition);
            }
        }
        Err(self)
    }

    fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.components.is_empty() {
            return Err(RecipeValidationError::EmptyMaskComposite);
        }
        if self.components.len() > MAX_MASK_COMPONENTS {
            return Err(RecipeValidationError::TooManyMaskComponents(
                self.components.len(),
            ));
        }
        if self.components[0].operation != MaskComponentOperation::Base {
            return Err(RecipeValidationError::FirstMaskComponentMustBeBase);
        }
        let mut component_ids = HashSet::with_capacity(self.components.len());
        for (index, component) in self.components.iter().enumerate() {
            if index > 0 && component.operation == MaskComponentOperation::Base {
                return Err(RecipeValidationError::BaseMaskComponentMustBeFirst);
            }
            if !component_ids.insert(component.id) {
                return Err(RecipeValidationError::DuplicateMaskComponent(component.id));
            }
            component.validate()?;
        }
        Ok(())
    }
}

const fn mask_component_enabled_default() -> bool {
    true
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
        /// Optional provider-neutral instruction that can regenerate a
        /// photo-specific variant when this mask is copied elsewhere.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        semantic_intent: Option<SemanticMaskIntent>,
        #[serde(default)]
        expansion_percent: i8,
        #[serde(default)]
        feather_percent: u8,
        #[serde(default)]
        invert: bool,
    },
    /// A bounded ordered composition of existing leaf masks. This variant is
    /// still one definition owned by one MaskRevision, not a second mask graph
    /// or independently referenced subsystem.
    Composite {
        #[serde(flatten)]
        composite: MaskComposite,
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

    /// Creates one bounded ordered mask composition.
    ///
    /// A single enabled Base component with no final inversion is normalized
    /// back to its legacy leaf definition. Existing one-mask Recipes therefore
    /// retain their original serialized byte shape until composition-only
    /// state is actually authored.
    ///
    /// # Errors
    ///
    /// Returns an error when the composition violates its count, identity,
    /// operation-order, nesting, or leaf-definition contract.
    pub fn composite(
        components: Vec<MaskComponent>,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        match MaskComposite::new(components, invert)?.into_single_definition() {
            Ok(definition) => Ok(definition),
            Err(composite) => Ok(Self::Composite { composite }),
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
        Self::managed_raster_with_semantic_intent_and_refinement(
            raster,
            None,
            expansion_percent,
            feather_percent,
            invert,
        )
    }

    /// Stores accepted raster bytes together with the semantic instruction
    /// used to produce them. Renderers consume only the raster; cross-photo
    /// authoring may rerun the intent and replace it with a new accepted one.
    ///
    /// # Errors
    ///
    /// Returns an error when the raster, semantic intent, or refinement
    /// values violate their fixed contracts.
    pub fn managed_raster_with_semantic_intent_and_refinement(
        raster: ManagedRasterMask,
        semantic_intent: Option<SemanticMaskIntent>,
        expansion_percent: i8,
        feather_percent: u8,
        invert: bool,
    ) -> Result<Self, RecipeValidationError> {
        let definition = Self::ManagedRaster {
            raster,
            semantic_intent,
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
                semantic_intent,
                expansion_percent,
                feather_percent,
                ..
            } => {
                raster.validate()?;
                if let Some(intent) = semantic_intent {
                    intent.validate()?;
                }
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
            Self::Composite { composite } => {
                composite.validate()?;
                if composite.is_reducible_to_single_definition() {
                    return Err(RecipeValidationError::NonCanonicalMaskComposite);
                }
            }
        }
        Ok(())
    }

    /// Returns the re-evaluable instruction attached to an accepted semantic
    /// raster, if this managed mask was created semantically.
    pub const fn semantic_intent(&self) -> Option<&SemanticMaskIntent> {
        match self {
            Self::ManagedRaster {
                semantic_intent, ..
            } => semantic_intent.as_ref(),
            _ => None,
        }
    }

    /// Returns the ordered composite owner when this definition actually
    /// contains composition-only authored state.
    pub const fn composite_definition(&self) -> Option<&MaskComposite> {
        match self {
            Self::Composite { composite } => Some(composite),
            _ => None,
        }
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

//! Bounded expression grammar, aggregate validation, and legacy-leaf projection.

use serde::{Deserialize, Deserializer, Serialize, de::Error as _};

use super::{ConditionMaskPredicate, predicate::validate_predicate};
use crate::recipe::{FiniteF64, RecipeValidationError, UnitInterval};

pub const CURRENT_CONDITION_MASK_SCHEMA_VERSION: u32 = 1;
pub const MAX_CONDITION_MASK_DEPTH: usize = 4;
pub const MAX_CONDITION_MASK_LEAVES: usize = 8;
pub const MAX_CONDITION_MASK_BRANCHES: usize = 4;

/// One versioned, bounded soft-mask expression.
///
/// The root is validated on construction and deserialization. A persisted
/// expression can therefore never smuggle an unbounded tree into a renderer,
/// cache key, or future model adapter.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ConditionMaskExpression {
    schema_version: u32,
    root: ConditionMaskNode,
}

#[derive(Deserialize)]
struct UnvalidatedConditionMaskExpression {
    schema_version: u32,
    root: ConditionMaskNode,
}

impl TryFrom<UnvalidatedConditionMaskExpression> for ConditionMaskExpression {
    type Error = RecipeValidationError;

    fn try_from(value: UnvalidatedConditionMaskExpression) -> Result<Self, Self::Error> {
        let expression = Self {
            schema_version: value.schema_version,
            root: value.root,
        };
        expression.validate()?;
        Ok(expression)
    }
}

impl<'de> Deserialize<'de> for ConditionMaskExpression {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        UnvalidatedConditionMaskExpression::deserialize(deserializer)?
            .try_into()
            .map_err(D::Error::custom)
    }
}

impl ConditionMaskExpression {
    /// Creates and validates a current-schema expression.
    ///
    /// # Errors
    ///
    /// Returns an error when the tree or a condition exceeds its fixed
    /// persistent bounds.
    pub fn new(root: ConditionMaskNode) -> Result<Self, RecipeValidationError> {
        Self {
            schema_version: CURRENT_CONDITION_MASK_SCHEMA_VERSION,
            root,
        }
        .checked()
    }

    /// Creates a one-predicate expression.
    ///
    /// # Errors
    ///
    /// Returns an error when the predicate violates its typed numeric domain.
    pub fn leaf(condition: ConditionMaskPredicate) -> Result<Self, RecipeValidationError> {
        Self::new(ConditionMaskNode::Leaf { condition })
    }

    /// Creates an intersection expression.
    ///
    /// # Errors
    ///
    /// Returns an error when fan-out, depth, leaf count, or a predicate is
    /// outside the fixed contract.
    pub fn all(children: Vec<ConditionMaskNode>) -> Result<Self, RecipeValidationError> {
        Self::new(ConditionMaskNode::All { children })
    }

    /// Creates a union expression.
    ///
    /// # Errors
    ///
    /// Returns an error when fan-out, depth, leaf count, or a predicate is
    /// outside the fixed contract.
    pub fn any(children: Vec<ConditionMaskNode>) -> Result<Self, RecipeValidationError> {
        Self::new(ConditionMaskNode::Any { children })
    }

    /// Creates a complemented expression.
    ///
    /// # Errors
    ///
    /// Returns an error when the resulting depth, leaf count, or predicate is
    /// outside the fixed contract.
    pub fn not(child: ConditionMaskNode) -> Result<Self, RecipeValidationError> {
        Self::new(ConditionMaskNode::Not {
            child: Box::new(child),
        })
    }

    pub const fn schema_version(&self) -> u32 {
        self.schema_version
    }

    pub const fn root(&self) -> &ConditionMaskNode {
        &self.root
    }

    /// Revalidates the complete persisted expression.
    ///
    /// # Errors
    ///
    /// Returns the first schema, shape, or leaf invariant violation.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.schema_version != CURRENT_CONDITION_MASK_SCHEMA_VERSION {
            return Err(
                RecipeValidationError::UnsupportedConditionMaskSchemaVersion {
                    expected: CURRENT_CONDITION_MASK_SCHEMA_VERSION,
                    actual: self.schema_version,
                },
            );
        }
        let mut leaves = 0;
        validate_node(&self.root, 1, &mut leaves)?;
        Ok(())
    }

    pub(crate) fn legacy_leaf(&self) -> Option<LegacyConditionLeaf> {
        legacy_leaf(&self.root, false)
    }

    fn checked(self) -> Result<Self, RecipeValidationError> {
        self.validate()?;
        Ok(self)
    }
}

/// The only recursive operators admitted by the v1 condition contract.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "operator", rename_all = "snake_case")]
pub enum ConditionMaskNode {
    Leaf { condition: ConditionMaskPredicate },
    All { children: Vec<ConditionMaskNode> },
    Any { children: Vec<ConditionMaskNode> },
    Not { child: Box<ConditionMaskNode> },
}

impl ConditionMaskNode {
    pub const fn leaf(condition: ConditionMaskPredicate) -> Self {
        Self::Leaf { condition }
    }

    pub fn all(children: Vec<Self>) -> Self {
        Self::All { children }
    }

    pub fn any(children: Vec<Self>) -> Self {
        Self::Any { children }
    }

    pub fn negated(child: Self) -> Self {
        Self::Not {
            child: Box::new(child),
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq)]
pub(crate) enum LegacyConditionLeaf {
    Luminance {
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
        invert: bool,
    },
    Hue {
        center_hue_degrees: FiniteF64,
        half_width_degrees: FiniteF64,
        softness: UnitInterval,
        invert: bool,
    },
}

fn legacy_leaf(node: &ConditionMaskNode, invert: bool) -> Option<LegacyConditionLeaf> {
    match node {
        ConditionMaskNode::Leaf {
            condition:
                ConditionMaskPredicate::OklabLightnessRange {
                    lower,
                    upper,
                    softness,
                },
        } => Some(LegacyConditionLeaf::Luminance {
            lower: *lower,
            upper: *upper,
            softness: *softness,
            invert,
        }),
        ConditionMaskNode::Leaf {
            condition:
                ConditionMaskPredicate::OklchHueRange {
                    center_hue_degrees,
                    half_width_degrees,
                    minimum_chroma,
                    softness,
                    ..
                },
        } if *minimum_chroma == UnitInterval::ZERO => Some(LegacyConditionLeaf::Hue {
            center_hue_degrees: *center_hue_degrees,
            half_width_degrees: *half_width_degrees,
            softness: *softness,
            invert,
        }),
        ConditionMaskNode::Not { child } => legacy_leaf(child, !invert),
        _ => None,
    }
}

fn validate_node(
    node: &ConditionMaskNode,
    depth: usize,
    leaves: &mut usize,
) -> Result<(), RecipeValidationError> {
    if depth > MAX_CONDITION_MASK_DEPTH {
        return Err(RecipeValidationError::ConditionMaskDepthExceeded(depth));
    }
    match node {
        ConditionMaskNode::Leaf { condition } => {
            *leaves += 1;
            if *leaves > MAX_CONDITION_MASK_LEAVES {
                return Err(RecipeValidationError::TooManyConditionMaskLeaves(*leaves));
            }
            validate_predicate(condition)
        }
        ConditionMaskNode::All { children } => validate_branch("all", children, depth, leaves),
        ConditionMaskNode::Any { children } => validate_branch("any", children, depth, leaves),
        ConditionMaskNode::Not { child } => validate_node(child, depth + 1, leaves),
    }
}

fn validate_branch(
    operator: &'static str,
    children: &[ConditionMaskNode],
    depth: usize,
    leaves: &mut usize,
) -> Result<(), RecipeValidationError> {
    if !(2..=MAX_CONDITION_MASK_BRANCHES).contains(&children.len()) {
        return Err(RecipeValidationError::InvalidConditionMaskBranchCount {
            operator,
            count: children.len(),
        });
    }
    for child in children {
        validate_node(child, depth + 1, leaves)?;
    }
    Ok(())
}

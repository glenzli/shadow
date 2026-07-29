//! Validated copy-only preset values.

use serde::{Deserialize, Serialize};

use super::ConditionMaskExpression;
use crate::recipe::{
    RecipeValidationError,
    value::{MAX_LABEL_BYTES, display_name_character, validate_text},
};

/// A user- or product-supplied conditional-mask preset.
///
/// Applying a preset copies [`ConditionMaskExpression`] parameters into a new
/// node-local mask. Recipes never retain this name or a live preset identity.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ConditionMaskPreset {
    name: ConditionMaskPresetName,
    expression: ConditionMaskExpression,
}

impl ConditionMaskPreset {
    /// Creates a validated copy-only preset.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid display text or expression content.
    pub fn new(
        name: impl Into<String>,
        expression: ConditionMaskExpression,
    ) -> Result<Self, RecipeValidationError> {
        expression.validate()?;
        Ok(Self {
            name: ConditionMaskPresetName::new(name)?,
            expression,
        })
    }

    pub fn name(&self) -> &str {
        self.name.as_str()
    }

    pub const fn expression(&self) -> &ConditionMaskExpression {
        &self.expression
    }

    /// Returns an owned parameter copy with no reference to this preset.
    pub fn copy_expression(&self) -> ConditionMaskExpression {
        self.expression.clone()
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(try_from = "String", into = "String")]
struct ConditionMaskPresetName(String);

impl ConditionMaskPresetName {
    fn new(value: impl Into<String>) -> Result<Self, RecipeValidationError> {
        let value = value.into();
        validate_text(
            &value,
            MAX_LABEL_BYTES,
            "condition mask preset name",
            display_name_character,
        )?;
        Ok(Self(value))
    }

    fn as_str(&self) -> &str {
        &self.0
    }
}

impl TryFrom<String> for ConditionMaskPresetName {
    type Error = RecipeValidationError;

    fn try_from(value: String) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<ConditionMaskPresetName> for String {
    fn from(value: ConditionMaskPresetName) -> Self {
        value.0
    }
}

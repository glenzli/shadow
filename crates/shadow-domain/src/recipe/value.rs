//! Portable Recipe scalar values, stable identifiers, and validated display names.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;

pub(super) const MAX_STABLE_NAME_BYTES: usize = 128;
pub(super) const MAX_LABEL_BYTES: usize = 512;
pub(super) const MAX_COMMIT_MESSAGE_BYTES: usize = 4_096;

/// A finite persisted floating-point value.
///
/// JSON cannot represent NaN or infinity consistently and those values also
/// make recipe equality and cache keys surprising, so they never enter the
/// domain model.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(try_from = "f64", into = "f64")]
pub struct FiniteF64(pub(super) f64);

impl FiniteF64 {
    /// Creates a portable finite value.
    ///
    /// # Errors
    ///
    /// Returns [`RecipeValidationError::NonFiniteNumber`] for NaN or infinity.
    pub fn new(value: f64) -> Result<Self, RecipeValidationError> {
        if value.is_finite() {
            Ok(Self(value))
        } else {
            Err(RecipeValidationError::NonFiniteNumber)
        }
    }

    pub const fn get(self) -> f64 {
        self.0
    }
}

pub(super) const fn default_finite_zero() -> FiniteF64 {
    FiniteF64(0.0)
}

impl TryFrom<f64> for FiniteF64 {
    type Error = RecipeValidationError;

    fn try_from(value: f64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<FiniteF64> for f64 {
    fn from(value: FiniteF64) -> Self {
        value.0
    }
}

/// A finite value in the closed interval `[0, 1]`.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(try_from = "f64", into = "f64")]
pub struct UnitInterval(pub(super) FiniteF64);

impl UnitInterval {
    pub const ZERO: Self = Self(FiniteF64(0.0));
    pub const ONE: Self = Self(FiniteF64(1.0));

    /// Creates a finite value in the closed interval `[0, 1]`.
    ///
    /// # Errors
    ///
    /// Returns an error when the value is non-finite or outside the interval.
    pub fn new(value: f64) -> Result<Self, RecipeValidationError> {
        let value = FiniteF64::new(value)?;
        if (0.0..=1.0).contains(&value.get()) {
            Ok(Self(value))
        } else {
            Err(RecipeValidationError::UnitIntervalOutOfRange(value.get()))
        }
    }

    pub const fn get(self) -> f64 {
        self.0.get()
    }
}

impl TryFrom<f64> for UnitInterval {
    type Error = RecipeValidationError;

    fn try_from(value: f64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<UnitInterval> for f64 {
    fn from(value: UnitInterval) -> Self {
        value.get()
    }
}

macro_rules! validated_string {
    ($name:ident, $max:expr, $kind:literal, $validator:expr) => {
        #[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
        #[serde(try_from = "String", into = "String")]
        pub struct $name(String);

        impl $name {
            /// Creates a validated persistent name.
            ///
            /// # Errors
            ///
            /// Returns an error for empty, oversized, padded, control, or
            /// type-specific unsupported text.
            pub fn new(value: impl Into<String>) -> Result<Self, RecipeValidationError> {
                let value = value.into();
                validate_text(&value, $max, $kind, $validator)?;
                Ok(Self(value))
            }

            pub fn as_str(&self) -> &str {
                &self.0
            }
        }

        impl TryFrom<String> for $name {
            type Error = RecipeValidationError;

            fn try_from(value: String) -> Result<Self, Self::Error> {
                Self::new(value)
            }
        }

        impl From<$name> for String {
            fn from(value: $name) -> Self {
                value.0
            }
        }
    };
}

pub(super) fn validate_text(
    value: &str,
    max_bytes: usize,
    kind: &'static str,
    extra: fn(char) -> bool,
) -> Result<(), RecipeValidationError> {
    if value.is_empty() || value.trim() != value {
        return Err(RecipeValidationError::InvalidText {
            kind,
            reason: "must be non-empty and have no surrounding whitespace",
        });
    }
    if value.len() > max_bytes {
        return Err(RecipeValidationError::TextTooLong {
            kind,
            max_bytes,
            actual_bytes: value.len(),
        });
    }
    if value
        .chars()
        .any(|character| character.is_control() || !extra(character))
    {
        return Err(RecipeValidationError::InvalidText {
            kind,
            reason: "contains unsupported characters",
        });
    }
    Ok(())
}

pub(super) fn stable_name_character(character: char) -> bool {
    character.is_ascii_alphanumeric() || matches!(character, '.' | '_' | '-' | '/' | ':')
}

pub(super) fn display_name_character(_character: char) -> bool {
    true
}

validated_string!(
    OperationId,
    MAX_STABLE_NAME_BYTES,
    "operation id",
    stable_name_character
);
validated_string!(
    ParameterKey,
    MAX_STABLE_NAME_BYTES,
    "parameter key",
    stable_name_character
);
validated_string!(
    BranchName,
    MAX_LABEL_BYTES,
    "branch name",
    display_name_character
);
validated_string!(
    VersionName,
    MAX_LABEL_BYTES,
    "version name",
    display_name_character
);

#[cfg(test)]
mod tests;

//! Mandatory photo-local source-development foundation.

use serde::{Deserialize, Serialize};

use super::{RecipeInputSettings, RecipeOpticsSettings, RecipeValidationError};

/// Fixed denominator used by persisted camera-neutral channel ratios.
///
/// `CameraNeutral` is defined only up to a common scale. Shadow fixes green to
/// this integer value so semantically equal manual white balances have one
/// JSON and cache identity across Rust, C++, preview, detail, and export.
pub const RAW_CAMERA_NEUTRAL_MILLIONTHS: u32 = 1_000_000;

const MIN_RAW_CAMERA_NEUTRAL_MILLIONTHS: u32 = RAW_CAMERA_NEUTRAL_MILLIONTHS / 64;
const MAX_RAW_CAMERA_NEUTRAL_MILLIONTHS: u32 = RAW_CAMERA_NEUTRAL_MILLIONTHS * 64;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
struct RawCameraNeutralWire {
    red_millionths: u32,
    blue_millionths: u32,
}

/// Canonical manual RAW white point in the source camera's RGB space.
///
/// DNG `CameraNeutral` components are scale-invariant. Shadow stores only red
/// and blue relative to an implicit green of exactly 1.0, quantized to one
/// millionth. This is an absolute source interpretation for one photo, not a
/// Kelvin value and not a creative RGB temperature offset.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(try_from = "RawCameraNeutralWire", into = "RawCameraNeutralWire")]
pub struct RawCameraNeutral {
    red_millionths: u32,
    blue_millionths: u32,
}

impl RawCameraNeutral {
    /// Creates a canonical camera neutral from positive RGB components.
    ///
    /// The input is normalized by green and rounded to one-millionth units.
    /// The intentionally broad `1/64..=64` ratio bound prevents reciprocal
    /// white-balance gains from becoming numerically pathological.
    ///
    /// # Errors
    ///
    /// Returns an error for non-finite, non-positive, or out-of-range channel
    /// ratios.
    pub fn new(red: f64, green: f64, blue: f64) -> Result<Self, RecipeValidationError> {
        if !green.is_finite() || green <= 0.0 {
            return Err(RecipeValidationError::InvalidRawCameraNeutral {
                component: "green",
                value: green,
            });
        }
        let normalized_red = red / green;
        let normalized_blue = blue / green;
        Self::from_normalized_components(normalized_red, normalized_blue)
    }

    /// Restores the exact persisted canonical representation.
    ///
    /// # Errors
    ///
    /// Returns an error when either component is outside the supported
    /// reciprocal-gain range.
    pub fn from_millionths(
        red_millionths: u32,
        blue_millionths: u32,
    ) -> Result<Self, RecipeValidationError> {
        for (component, value) in [("red", red_millionths), ("blue", blue_millionths)] {
            if !(MIN_RAW_CAMERA_NEUTRAL_MILLIONTHS..=MAX_RAW_CAMERA_NEUTRAL_MILLIONTHS)
                .contains(&value)
            {
                return Err(RecipeValidationError::InvalidRawCameraNeutral {
                    component,
                    value: f64::from(value) / f64::from(RAW_CAMERA_NEUTRAL_MILLIONTHS),
                });
            }
        }
        Ok(Self {
            red_millionths,
            blue_millionths,
        })
    }

    fn from_normalized_components(red: f64, blue: f64) -> Result<Self, RecipeValidationError> {
        Self::from_millionths(
            Self::quantize_normalized_component("red", red)?,
            Self::quantize_normalized_component("blue", blue)?,
        )
    }

    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    fn quantize_normalized_component(
        component: &'static str,
        value: f64,
    ) -> Result<u32, RecipeValidationError> {
        if !value.is_finite() || value <= 0.0 || !(1.0 / 64.0..=64.0).contains(&value) {
            return Err(RecipeValidationError::InvalidRawCameraNeutral { component, value });
        }
        // Validation bounds the rounded value to 15_625..=64_000_000, so the
        // conversion is finite, positive, and exactly representable by u32.
        Ok((value * f64::from(RAW_CAMERA_NEUTRAL_MILLIONTHS)).round() as u32)
    }

    pub const fn red_millionths(self) -> u32 {
        self.red_millionths
    }

    pub const fn green_millionths(self) -> u32 {
        RAW_CAMERA_NEUTRAL_MILLIONTHS
    }

    pub const fn blue_millionths(self) -> u32 {
        self.blue_millionths
    }

    pub fn red(self) -> f64 {
        f64::from(self.red_millionths) / f64::from(RAW_CAMERA_NEUTRAL_MILLIONTHS)
    }

    pub const fn green(self) -> f64 {
        1.0
    }

    pub fn blue(self) -> f64 {
        f64::from(self.blue_millionths) / f64::from(RAW_CAMERA_NEUTRAL_MILLIONTHS)
    }
}

impl TryFrom<RawCameraNeutralWire> for RawCameraNeutral {
    type Error = RecipeValidationError;

    fn try_from(value: RawCameraNeutralWire) -> Result<Self, Self::Error> {
        Self::from_millionths(value.red_millionths, value.blue_millionths)
    }
}

impl From<RawCameraNeutral> for RawCameraNeutralWire {
    fn from(value: RawCameraNeutral) -> Self {
        Self {
            red_millionths: value.red_millionths,
            blue_millionths: value.blue_millionths,
        }
    }
}

/// Absolute RAW white-balance interpretation owned by the Foundation.
///
/// `AsShot` consumes the source metadata. `CameraNeutral` stores one
/// photographer-selected camera-space neutral. Automatic estimation is not a
/// variant until Shadow owns a versioned, reproducible algorithm; unsupported
/// intent therefore cannot be persisted and silently rendered as `AsShot`.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "mode", rename_all = "snake_case")]
pub enum RawWhiteBalance {
    #[default]
    AsShot,
    CameraNeutral {
        neutral: RawCameraNeutral,
    },
}

impl RawWhiteBalance {
    pub const fn camera_neutral(neutral: RawCameraNeutral) -> Self {
        Self::CameraNeutral { neutral }
    }

    pub const fn is_as_shot(&self) -> bool {
        matches!(self, Self::AsShot)
    }

    pub const fn neutral(self) -> Option<RawCameraNeutral> {
        match self {
            Self::AsShot => None,
            Self::CameraNeutral { neutral } => Some(neutral),
        }
    }
}

/// The mandatory, non-shareable source-development node for one photo.
///
/// Every Recipe owns exactly one Foundation node. Its role is stable and
/// cannot be deleted, reordered, duplicated, masked, or shared like a Grade
/// Node. Resetting the Foundation replaces its parameters with
/// [`Self::default`]; it does not remove the slot.
///
/// Foundation parameters describe absolute source interpretation. Repeatable
/// relative adjustments such as authored exposure compensation remain Grade
/// operations even when the desktop presents both groups in one inspector.
///
/// The transparent representation preserves the existing Recipe v1
/// `input_settings` payload while the in-memory model gains an explicit node
/// boundary. Future RAW white-balance and source-development contracts belong
/// here rather than in the repeatable Grade graph.
#[derive(Debug, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct PhotoFoundationNode {
    input_settings: RecipeInputSettings,
}

impl PhotoFoundationNode {
    /// Wraps the existing Recipe v1 source settings in their singleton role.
    pub const fn new(input_settings: RecipeInputSettings) -> Self {
        Self { input_settings }
    }

    /// Returns the complete source-development settings.
    pub const fn input_settings(&self) -> &RecipeInputSettings {
        &self.input_settings
    }

    /// Returns the singleton optical profile and residual-correction pipeline.
    pub const fn optics(&self) -> &RecipeOpticsSettings {
        self.input_settings.optics()
    }

    /// Returns the absolute RAW white-balance source interpretation.
    pub const fn raw_white_balance(&self) -> RawWhiteBalance {
        self.input_settings.raw_white_balance()
    }

    /// Removes the node role and returns its Recipe v1 compatibility value.
    pub fn into_input_settings(self) -> RecipeInputSettings {
        self.input_settings
    }

    pub(super) fn is_default(&self) -> bool {
        self.input_settings.is_default()
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        self.optics().validate()
    }
}

impl From<RecipeInputSettings> for PhotoFoundationNode {
    fn from(input_settings: RecipeInputSettings) -> Self {
        Self::new(input_settings)
    }
}

#[cfg(test)]
mod tests;

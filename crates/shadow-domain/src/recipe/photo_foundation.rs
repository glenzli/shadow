//! Mandatory photo-local source-development foundation.

use serde::{Deserialize, Serialize};

use super::{RecipeInputSettings, RecipeOpticsSettings, RecipeValidationError};

pub const RAW_WHITE_BALANCE_MIN_TEMPERATURE_KELVIN: u32 = 2_000;
pub const RAW_WHITE_BALANCE_MAX_TEMPERATURE_KELVIN: u32 = 25_000;
pub const RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN: u32 = 5_500;
pub const RAW_WHITE_BALANCE_MIN_TINT: i16 = -150;
pub const RAW_WHITE_BALANCE_MAX_TINT: i16 = 150;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
struct RawTemperatureTintWire {
    temperature_kelvin: u32,
    tint: i16,
}

/// Canonical photographer-facing absolute RAW white point.
///
/// Temperature is a correlated colour temperature in Kelvin. Tint uses the
/// conventional photographic green-to-magenta axis: negative values move
/// toward green and positive values move toward magenta. Camera-space channel
/// ratios are a renderer-derived implementation detail and are deliberately
/// not part of the Recipe or desktop editing contract.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(try_from = "RawTemperatureTintWire", into = "RawTemperatureTintWire")]
pub struct RawTemperatureTint {
    temperature_kelvin: u32,
    tint: i16,
}

impl RawTemperatureTint {
    /// Creates a bounded absolute white-balance value.
    ///
    /// # Errors
    ///
    /// Returns an error when temperature or tint is outside Shadow's
    /// reproducible photographic authoring range.
    pub fn new(temperature_kelvin: u32, tint: i16) -> Result<Self, RecipeValidationError> {
        if !(RAW_WHITE_BALANCE_MIN_TEMPERATURE_KELVIN..=RAW_WHITE_BALANCE_MAX_TEMPERATURE_KELVIN)
            .contains(&temperature_kelvin)
        {
            return Err(RecipeValidationError::InvalidRawWhiteBalanceTemperature(
                temperature_kelvin,
            ));
        }
        if !(RAW_WHITE_BALANCE_MIN_TINT..=RAW_WHITE_BALANCE_MAX_TINT).contains(&tint) {
            return Err(RecipeValidationError::InvalidRawWhiteBalanceTint(tint));
        }
        Ok(Self {
            temperature_kelvin,
            tint,
        })
    }

    pub const fn temperature_kelvin(self) -> u32 {
        self.temperature_kelvin
    }

    pub const fn tint(self) -> i16 {
        self.tint
    }
}

impl TryFrom<RawTemperatureTintWire> for RawTemperatureTint {
    type Error = RecipeValidationError;

    fn try_from(value: RawTemperatureTintWire) -> Result<Self, Self::Error> {
        Self::new(value.temperature_kelvin, value.tint)
    }
}

impl From<RawTemperatureTint> for RawTemperatureTintWire {
    fn from(value: RawTemperatureTint) -> Self {
        Self {
            temperature_kelvin: value.temperature_kelvin,
            tint: value.tint,
        }
    }
}

/// Absolute RAW white-balance interpretation owned by the Foundation.
///
/// `AsShot` is the compact default/reset state and consumes source metadata.
/// `TemperatureTint` stores the directly authored absolute photographic value.
/// The desktop presents one continuous pair of controls; it does not expose
/// these persistence variants as user-facing modes.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "mode", rename_all = "snake_case")]
pub enum RawWhiteBalance {
    #[default]
    AsShot,
    TemperatureTint {
        value: RawTemperatureTint,
    },
}

impl RawWhiteBalance {
    pub const fn temperature_tint(value: RawTemperatureTint) -> Self {
        Self::TemperatureTint { value }
    }

    pub const fn is_as_shot(&self) -> bool {
        matches!(self, Self::AsShot)
    }

    pub const fn authored_value(self) -> Option<RawTemperatureTint> {
        match self {
            Self::AsShot => None,
            Self::TemperatureTint { value } => Some(value),
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

    /// Returns whether optional Foundation interpretation is evaluated.
    ///
    /// Required source decoding remains active when this is false; authored
    /// white balance and optics values stay stored but are bypassed. The
    /// sibling AI RAW denoise node remains independently enabled or bypassed.
    pub const fn enabled(&self) -> bool {
        self.input_settings.enabled()
    }

    /// Returns the absolute RAW white-balance source interpretation.
    pub const fn raw_white_balance(&self) -> RawWhiteBalance {
        self.input_settings.raw_white_balance()
    }

    pub const fn effective_raw_white_balance(&self) -> RawWhiteBalance {
        if self.enabled() {
            self.raw_white_balance()
        } else {
            RawWhiteBalance::AsShot
        }
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

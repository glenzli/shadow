//! Recipe v1 source-setting payloads retained inside the Photo Foundation.

use serde::{Deserialize, Serialize};

use super::value::{MAX_LABEL_BYTES, display_name_character, validate_text};
use super::{RawFoundationDenoise, RawWhiteBalance, RecipeValidationError};

/// Foundation-owned optical corrections applied before creative Grade Nodes.
///
/// Profile selection and the profile-independent manual residuals form one
/// ordered singleton pipeline for the photo. They never belong to a reusable
/// Grade Node; a deliberately creative, stackable lens distortion would need
/// a separately named Grade operation.
const fn default_manual_vignetting_midpoint() -> u8 {
    50
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(default)]
#[allow(clippy::struct_excessive_bools)] // Each persisted switch controls an independent correction.
pub struct RecipeOpticsSettings {
    enabled: bool,
    correct_distortion: bool,
    correct_tca: bool,
    correct_vignetting: bool,
    automatic_scale: bool,
    /// Profile-independent residual corrections in integer percent units.
    /// They are input transforms, so they must remain exactly comparable for
    /// Recipe identity and must not become floating point Grade-node values.
    #[serde(default)]
    manual_distortion: i16,
    #[serde(default)]
    manual_tca_red_cyan: i16,
    #[serde(default)]
    manual_tca_blue_yellow: i16,
    #[serde(default)]
    manual_vignetting_amount: i16,
    #[serde(default = "default_manual_vignetting_midpoint")]
    manual_vignetting_midpoint: u8,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    camera_profile_maker: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    camera_profile_model: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    lens_profile_maker: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    lens_profile_model: String,
}

impl Default for RecipeOpticsSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            correct_distortion: true,
            correct_tca: true,
            correct_vignetting: true,
            automatic_scale: true,
            manual_distortion: 0,
            manual_tca_red_cyan: 0,
            manual_tca_blue_yellow: 0,
            manual_vignetting_amount: 0,
            manual_vignetting_midpoint: default_manual_vignetting_midpoint(),
            camera_profile_maker: String::new(),
            camera_profile_model: String::new(),
            lens_profile_maker: String::new(),
            lens_profile_model: String::new(),
        }
    }
}

impl RecipeOpticsSettings {
    #[allow(clippy::fn_params_excessive_bools)] // Mirrors the explicit persisted correction switches.
    pub fn new(
        enabled: bool,
        correct_distortion: bool,
        correct_tca: bool,
        correct_vignetting: bool,
        automatic_scale: bool,
    ) -> Self {
        Self {
            enabled,
            correct_distortion,
            correct_tca,
            correct_vignetting,
            automatic_scale,
            ..Self::default()
        }
    }

    #[must_use]
    pub fn with_manual_profile(
        mut self,
        camera_maker: impl Into<String>,
        camera_model: impl Into<String>,
        lens_maker: impl Into<String>,
        lens_model: impl Into<String>,
    ) -> Self {
        self.camera_profile_maker = camera_maker.into();
        self.camera_profile_model = camera_model.into();
        self.lens_profile_maker = lens_maker.into();
        self.lens_profile_model = lens_model.into();
        self
    }

    #[must_use]
    pub const fn with_manual_corrections(
        mut self,
        distortion: i16,
        tca_red_cyan: i16,
        tca_blue_yellow: i16,
        vignetting_amount: i16,
        vignetting_midpoint: u8,
    ) -> Self {
        self.manual_distortion = distortion;
        self.manual_tca_red_cyan = tca_red_cyan;
        self.manual_tca_blue_yellow = tca_blue_yellow;
        self.manual_vignetting_amount = vignetting_amount;
        self.manual_vignetting_midpoint = vignetting_midpoint;
        self
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    pub const fn correct_distortion(&self) -> bool {
        self.correct_distortion
    }

    pub const fn correct_tca(&self) -> bool {
        self.correct_tca
    }

    pub const fn correct_vignetting(&self) -> bool {
        self.correct_vignetting
    }

    pub const fn automatic_scale(&self) -> bool {
        self.automatic_scale
    }

    pub const fn manual_distortion(&self) -> i16 {
        self.manual_distortion
    }

    pub const fn manual_tca_red_cyan(&self) -> i16 {
        self.manual_tca_red_cyan
    }

    pub const fn manual_tca_blue_yellow(&self) -> i16 {
        self.manual_tca_blue_yellow
    }

    pub const fn manual_vignetting_amount(&self) -> i16 {
        self.manual_vignetting_amount
    }

    pub const fn manual_vignetting_midpoint(&self) -> u8 {
        self.manual_vignetting_midpoint
    }

    pub fn camera_profile_maker(&self) -> &str {
        &self.camera_profile_maker
    }
    pub fn camera_profile_model(&self) -> &str {
        &self.camera_profile_model
    }
    pub fn lens_profile_maker(&self) -> &str {
        &self.lens_profile_maker
    }
    pub fn lens_profile_model(&self) -> &str {
        &self.lens_profile_model
    }
    pub fn uses_manual_profile(&self) -> bool {
        !self.camera_profile_model.is_empty() && !self.lens_profile_model.is_empty()
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        for (kind, value) in [
            ("manual distortion", self.manual_distortion),
            (
                "manual red/cyan chromatic aberration",
                self.manual_tca_red_cyan,
            ),
            (
                "manual blue/yellow chromatic aberration",
                self.manual_tca_blue_yellow,
            ),
            ("manual optical vignetting", self.manual_vignetting_amount),
        ] {
            if !(-100..=100).contains(&value) {
                return Err(RecipeValidationError::InvalidOpticsManualValue { kind, value });
            }
        }
        if self.manual_vignetting_midpoint > 100 {
            return Err(RecipeValidationError::InvalidOpticsVignettingMidpoint(
                self.manual_vignetting_midpoint,
            ));
        }
        for (kind, value) in [
            ("camera profile maker", self.camera_profile_maker.as_str()),
            ("camera profile model", self.camera_profile_model.as_str()),
            ("lens profile maker", self.lens_profile_maker.as_str()),
            ("lens profile model", self.lens_profile_model.as_str()),
        ] {
            if !value.is_empty() {
                validate_text(value, MAX_LABEL_BYTES, kind, display_name_character)?;
            }
        }
        let has_camera = !self.camera_profile_model.is_empty();
        let has_lens = !self.lens_profile_model.is_empty();
        let has_orphan_maker = (!self.camera_profile_maker.is_empty() && !has_camera)
            || (!self.lens_profile_maker.is_empty() && !has_lens);
        if has_camera != has_lens || has_orphan_maker {
            return Err(RecipeValidationError::IncompleteOpticsProfile);
        }
        Ok(())
    }
}

/// Recipe v1 compatibility payload stored by [`super::PhotoFoundationNode`].
///
/// New aggregate code should model the mandatory Foundation role explicitly.
/// This value remains public because desktop and persistence adapters still
/// project the unchanged `input_settings` JSON shape.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RecipeInputSettings {
    #[serde(default = "foundation_enabled", skip_serializing_if = "bool_is_true")]
    enabled: bool,
    #[serde(default)]
    optics: RecipeOpticsSettings,
    #[serde(default, skip_serializing_if = "RawWhiteBalance::is_as_shot")]
    raw_white_balance: RawWhiteBalance,
    // Recipe v1 keeps this sibling singleton in the historical
    // `input_settings` wire object. Semantic ownership belongs to
    // `RecipeSnapshot::raw_ai_denoise_node`, not the Foundation enable switch.
    #[serde(
        default,
        skip_serializing_if = "RawFoundationDenoise::is_default_state"
    )]
    raw_ai_denoise: RawFoundationDenoise,
}

impl RecipeInputSettings {
    pub const fn new(optics: RecipeOpticsSettings) -> Self {
        Self {
            enabled: true,
            optics,
            raw_white_balance: RawWhiteBalance::AsShot,
            raw_ai_denoise: RawFoundationDenoise::disabled(),
        }
    }

    #[must_use]
    pub const fn with_enabled(mut self, enabled: bool) -> Self {
        self.enabled = enabled;
        self
    }

    #[must_use]
    pub const fn with_raw_white_balance(mut self, raw_white_balance: RawWhiteBalance) -> Self {
        self.raw_white_balance = raw_white_balance;
        self
    }

    #[must_use]
    pub const fn with_raw_ai_denoise(mut self, raw_ai_denoise: RawFoundationDenoise) -> Self {
        self.raw_ai_denoise = raw_ai_denoise;
        self
    }

    pub const fn optics(&self) -> &RecipeOpticsSettings {
        &self.optics
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    pub const fn raw_white_balance(&self) -> RawWhiteBalance {
        self.raw_white_balance
    }

    pub const fn raw_ai_denoise(&self) -> RawFoundationDenoise {
        self.raw_ai_denoise
    }

    pub(super) fn is_default(&self) -> bool {
        *self == Self::default()
    }
}

impl Default for RecipeInputSettings {
    fn default() -> Self {
        Self::new(RecipeOpticsSettings::default())
    }
}

const fn foundation_enabled() -> bool {
    true
}

const fn bool_is_true(value: &bool) -> bool {
    *value
}

#[cfg(test)]
mod tests;

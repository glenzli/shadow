//! Non-destructive, photo-local AI RAW denoise singleton.
//!
//! This module stores only reproducible edit intent. Local model paths,
//! materialization progress, and rebuildable foundation-artifact locations are
//! runtime concerns and must not enter Recipe identity.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;

pub const RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT: u8 = 100;

const fn default_amount_percent() -> u8 {
    RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT
}

const fn amount_is_full(value: &u8) -> bool {
    *value == RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT
}

/// Exact public model contract admitted by the current Recipe schema.
///
/// The variant name pins the upstream release. The package and graph digests
/// are exposed separately so runtime registries and cache identities can reject
/// a file that merely claims to implement this model.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawFoundationDenoiseModel {
    #[default]
    RawNindPublicBayerRelease5_6_0,
}

impl RawFoundationDenoiseModel {
    pub const RAWNIND_PACKAGE_SHA256: &'static str =
        "d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af";
    pub const RAWNIND_BAYER_GRAPH_SHA256: &'static str =
        "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901";
    pub const RAWNIND_IMPLEMENTATION_REVISION: &'static str =
        "rawnind-public-bayer-foundation-20260731.1";

    /// Stable product-facing model identifier, independent of local paths.
    pub const fn identity(self) -> &'static str {
        match self {
            Self::RawNindPublicBayerRelease5_6_0 => "rawnind-public-bayer-release-5.6.0",
        }
    }

    pub const fn package_sha256(self) -> &'static str {
        match self {
            Self::RawNindPublicBayerRelease5_6_0 => Self::RAWNIND_PACKAGE_SHA256,
        }
    }

    pub const fn graph_sha256(self) -> &'static str {
        match self {
            Self::RawNindPublicBayerRelease5_6_0 => Self::RAWNIND_BAYER_GRAPH_SHA256,
        }
    }

    pub const fn implementation_revision(self) -> &'static str {
        match self {
            Self::RawNindPublicBayerRelease5_6_0 => Self::RAWNIND_IMPLEMENTATION_REVISION,
        }
    }
}

/// One photo-local, single-use AI RAW denoise node.
///
/// The node always has one stable slot before the Photo Foundation. Disabling
/// it bypasses the materialized result without deleting either the model
/// choice, authored amount, or rebuildable cached artifact. It cannot be
/// duplicated, reordered, masked, or shared like a Grade Node.
///
/// `amount_percent` blends the original RAW reconstruction with the already
/// materialized AI result in camera-linear RGB. It never changes model
/// execution or foundation-artifact identity.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct RawFoundationDenoise {
    enabled: bool,
    model: RawFoundationDenoiseModel,
    #[serde(
        default = "default_amount_percent",
        skip_serializing_if = "amount_is_full"
    )]
    amount_percent: u8,
}

impl Default for RawFoundationDenoise {
    fn default() -> Self {
        Self::disabled()
    }
}

impl RawFoundationDenoise {
    pub const fn disabled() -> Self {
        Self {
            enabled: false,
            model: RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
            amount_percent: RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT,
        }
    }

    pub const fn enabled(model: RawFoundationDenoiseModel) -> Self {
        Self {
            enabled: true,
            model,
            amount_percent: RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT,
        }
    }

    #[must_use]
    pub const fn with_enabled(mut self, enabled: bool) -> Self {
        self.enabled = enabled;
        self
    }

    pub const fn is_enabled(self) -> bool {
        self.enabled
    }

    pub const fn model(self) -> RawFoundationDenoiseModel {
        self.model
    }

    /// Sets the authored blend amount without changing the cached AI result.
    ///
    /// # Errors
    ///
    /// Returns an error when `amount_percent` exceeds 100.
    pub fn with_amount_percent(
        mut self,
        amount_percent: u8,
    ) -> Result<Self, RecipeValidationError> {
        if amount_percent > RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT {
            return Err(RecipeValidationError::InvalidRawFoundationDenoiseAmount(
                amount_percent,
            ));
        }
        self.amount_percent = amount_percent;
        Ok(self)
    }

    pub const fn amount_percent(self) -> u8 {
        self.amount_percent
    }

    /// Whether rendering needs the materialized AI artifact.
    pub const fn is_effective(self) -> bool {
        self.enabled && self.amount_percent > 0
    }

    pub(super) fn validate(self) -> Result<(), RecipeValidationError> {
        if self.amount_percent > RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT {
            return Err(RecipeValidationError::InvalidRawFoundationDenoiseAmount(
                self.amount_percent,
            ));
        }
        Ok(())
    }

    #[allow(clippy::trivially_copy_pass_by_ref)] // Serde skip_serializing_if requires `fn(&T)`.
    pub(super) const fn is_default_state(&self) -> bool {
        !self.enabled
            && matches!(
                self.model,
                RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0
            )
            && self.amount_percent == RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT
    }
}

#[cfg(test)]
mod tests;

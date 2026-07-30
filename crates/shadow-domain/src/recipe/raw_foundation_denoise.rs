//! Non-destructive AI RAW denoise intent owned by the Photo Foundation.
//!
//! This module stores only reproducible edit intent. Local model paths,
//! materialization progress, and rebuildable foundation-artifact locations are
//! runtime concerns and must not enter Recipe identity.

use serde::{Deserialize, Serialize};

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
/// The node always has one stable slot inside the Photo Foundation. Disabling
/// it bypasses the materialized foundation without deleting either the model
/// choice or a rebuildable cached result. It cannot be duplicated, reordered,
/// masked, or shared like a Grade Node.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct RawFoundationDenoise {
    enabled: bool,
    model: RawFoundationDenoiseModel,
}

impl RawFoundationDenoise {
    pub const fn disabled() -> Self {
        Self {
            enabled: false,
            model: RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
        }
    }

    pub const fn enabled(model: RawFoundationDenoiseModel) -> Self {
        Self {
            enabled: true,
            model,
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

    #[allow(clippy::trivially_copy_pass_by_ref)] // Serde skip_serializing_if requires `fn(&T)`.
    pub(super) const fn is_disabled(&self) -> bool {
        !self.enabled
    }
}

#[cfg(test)]
mod tests;

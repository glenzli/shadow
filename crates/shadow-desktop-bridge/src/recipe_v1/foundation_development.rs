//! Compiles the Photo Foundation into intent-specific RAW source development.
//!
//! Preview, detail, and export choose different quality policies, but the
//! photo-private absolute white balance must be identical across all three.
//! This owner also protects authored absolute RAW white balance from silently
//! crossing an RGB compatibility route that can no longer reproduce it.

use anyhow::{Result as AnyResult, bail};
use shadow_bridge::{OpticsSettings, RawDevelopmentPlan, RawPipelinePath, RawPipelineReceipt};
use shadow_domain::{
    PhotoFoundationNode, RawFoundationDenoise, RawWhiteBalance, RecipeOpticsSettings,
    RecipeSnapshot,
};

/// Immutable source-development contract compiled from the same Recipe
/// snapshot as the downstream adjustment plan.
///
/// Preview, detail, and export must consume this value rather than rebuilding
/// Foundation state from a mutable desktop DTO. That prevents a frame whose
/// Recipe identity names one source interpretation while native preparation
/// executes another.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct ResolvedFoundationDevelopment {
    white_balance: RawWhiteBalance,
    raw_ai_denoise: RawFoundationDenoise,
    optics: OpticsSettings,
}

impl ResolvedFoundationDevelopment {
    pub(crate) fn from_snapshot(snapshot: &RecipeSnapshot) -> Self {
        Self::from_foundation(snapshot.foundation_node(), snapshot.raw_ai_denoise_node())
    }

    fn from_foundation(
        foundation: &PhotoFoundationNode,
        raw_ai_denoise: RawFoundationDenoise,
    ) -> Self {
        Self {
            white_balance: foundation.effective_raw_white_balance(),
            raw_ai_denoise,
            optics: resolved_optics_settings(foundation),
        }
    }

    pub(crate) const fn raw_ai_denoise(&self) -> RawFoundationDenoise {
        self.raw_ai_denoise
    }

    pub(crate) const fn optics(&self) -> &OpticsSettings {
        &self.optics
    }

    pub(crate) const fn preview_plan(&self) -> RawDevelopmentPlan {
        preview_foundation_development_plan(self.white_balance)
    }

    pub(crate) const fn detail_plan(&self) -> RawDevelopmentPlan {
        detail_foundation_development_plan(self.white_balance)
    }

    pub(crate) const fn export_plan(&self) -> RawDevelopmentPlan {
        export_foundation_development_plan(self.white_balance)
    }
}

fn resolved_optics_settings(foundation: &PhotoFoundationNode) -> OpticsSettings {
    let settings: &RecipeOpticsSettings = foundation.optics();
    OpticsSettings {
        enabled: foundation.enabled() && settings.enabled(),
        correct_distortion: settings.correct_distortion(),
        correct_tca: settings.correct_tca(),
        correct_vignetting: settings.correct_vignetting(),
        automatic_scale: settings.automatic_scale(),
        manual_distortion: settings.manual_distortion(),
        manual_tca_red_cyan: settings.manual_tca_red_cyan(),
        manual_tca_blue_yellow: settings.manual_tca_blue_yellow(),
        manual_vignetting_amount: settings.manual_vignetting_amount(),
        manual_vignetting_midpoint: settings.manual_vignetting_midpoint(),
        camera_profile_maker: settings.camera_profile_maker().to_owned(),
        camera_profile_model: settings.camera_profile_model().to_owned(),
        lens_profile_maker: settings.lens_profile_maker().to_owned(),
        lens_profile_model: settings.lens_profile_model().to_owned(),
    }
}

pub(crate) const fn preview_foundation_development_plan(
    white_balance: RawWhiteBalance,
) -> RawDevelopmentPlan {
    RawDevelopmentPlan::preview().with_white_balance(white_balance)
}

pub(crate) const fn detail_foundation_development_plan(
    white_balance: RawWhiteBalance,
) -> RawDevelopmentPlan {
    RawDevelopmentPlan::detail().with_white_balance(white_balance)
}

pub(crate) const fn export_foundation_development_plan(
    white_balance: RawWhiteBalance,
) -> RawDevelopmentPlan {
    RawDevelopmentPlan::export_image().with_white_balance(white_balance)
}

pub(crate) fn ensure_foundation_development_receipt(
    requested_plan: RawDevelopmentPlan,
    receipt: &RawPipelineReceipt,
) -> AnyResult<()> {
    if receipt.requested_plan != requested_plan {
        bail!("prepared source does not record the requested Foundation development plan");
    }
    if requested_plan.white_balance.is_as_shot() {
        return Ok(());
    }
    if receipt.path != RawPipelinePath::ShadowRawFrame
        || receipt.effective_plan.white_balance != requested_plan.white_balance
    {
        bail!("manual Foundation RAW white balance requires Shadow's sensor-domain RawFrame path");
    }
    Ok(())
}

pub(crate) fn ensure_foundation_allows_rgb_fallback(
    requested_plan: RawDevelopmentPlan,
) -> AnyResult<()> {
    if !requested_plan.white_balance.is_as_shot() {
        bail!(
            "manual Foundation RAW white balance cannot use an isolated provider-processed RGB fallback"
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests;

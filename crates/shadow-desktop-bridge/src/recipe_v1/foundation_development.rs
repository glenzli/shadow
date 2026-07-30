//! Compiles the Photo Foundation into intent-specific RAW source development.
//!
//! Preview, detail, and export choose different quality policies, but the
//! photo-private absolute white balance must be identical across all three.
//! This owner also protects manual camera-space white balance from silently
//! crossing an RGB compatibility route that can no longer reproduce it.

use anyhow::{Result as AnyResult, bail};
use shadow_bridge::{RawDevelopmentPlan, RawPipelinePath, RawPipelineReceipt};
use shadow_domain::RawWhiteBalance;

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

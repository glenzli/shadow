//! Resolves one queued desktop edit into an immutable executable Recipe.
//!
//! Preview, detail, and export intentionally share this boundary. Slider state
//! and the explicitly captured base commit form one render generation; the
//! movable working ref must never be resolved after that generation is queued.

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_bridge::AdjustmentRenderPlan;
use shadow_catalog::CatalogHandle;
use shadow_domain::{PhotoId, RawWhiteBalance, RecipeCommitId};

use crate::ffi;

use super::{
    compile_recipe_render_plan, grade_stack_recipe_v1_snapshot, preview_grade_stack_draft_recipe_v1,
};

#[derive(Debug)]
pub(crate) struct ResolvedRecipeRender {
    pub(crate) plan: AdjustmentRenderPlan,
    pub(crate) snapshot_digest: [u8; 32],
    /// Absolute source interpretation captured from the same immutable
    /// snapshot as the downstream Grade render plan.
    pub(crate) raw_white_balance: RawWhiteBalance,
}

pub(crate) fn resolve_recipe_render(
    catalog: &CatalogHandle,
    photo_id: PhotoId,
    base_commit_id: &str,
    settings: &ffi::FfiEditSettings,
    use_working_recipe: bool,
) -> AnyResult<ResolvedRecipeRender> {
    let grade_stack = preview_grade_stack_draft_recipe_v1(settings, use_working_recipe)?;
    // Sliders and their immutable base commit travel as one render
    // generation. Never resolve the movable working ref here: it may have
    // advanced while this worker was queued, which would create a hybrid
    // Recipe that never existed in version history.
    let base_commit = if use_working_recipe && !base_commit_id.is_empty() {
        let commit_id: RecipeCommitId = base_commit_id
            .parse()
            .with_context(|| format!("parse preview base commit id {base_commit_id}"))?;
        Some(
            catalog
                .recipe_commit(photo_id, commit_id)?
                .ok_or_else(|| anyhow!("preview base Recipe commit {commit_id} is unavailable"))?,
        )
    } else {
        None
    };
    let template = base_commit.as_ref().map(|record| record.commit.snapshot());
    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, template)?;
    let snapshot_digest = shadow_domain::canonical_recipe_snapshot_digest(&snapshot)
        .context("serialize exact Recipe render identity")?;
    let raw_white_balance = snapshot.foundation_node().raw_white_balance();
    Ok(ResolvedRecipeRender {
        plan: compile_recipe_render_plan(&snapshot)?,
        snapshot_digest,
        raw_white_balance,
    })
}

#[cfg(test)]
mod tests;

//! Resolves one queued desktop edit into an immutable executable Recipe.
//!
//! Preview, detail, and export intentionally share this boundary. Slider state
//! and the explicitly captured base commit form one render generation; the
//! movable working ref must never be resolved after that generation is queued.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_bridge::AdjustmentRenderPlan;
use shadow_catalog::CatalogHandle;
use shadow_domain::{PhotoId, RecipeCommitId};

use crate::ffi;

use super::{
    GradeStackDraft, ResolvedFoundationDevelopment,
    compile_recipe_render_plan_with_managed_rasters, encode_grade_stack_draft_recipe_v1,
    grade_stack_recipe_v1_snapshot, managed_raster_resolution::FilesystemManagedRasterMaskResolver,
    preview_grade_stack_draft_recipe_v1,
};

#[derive(Debug)]
pub(crate) struct ResolvedRecipeRender {
    pub(crate) plan: AdjustmentRenderPlan,
    pub(crate) snapshot_digest: [u8; 32],
    /// Complete source interpretation captured from the same immutable
    /// snapshot as the downstream Grade render plan.
    pub(crate) foundation: ResolvedFoundationDevelopment,
}

/// Colour-neutral comparison at the current composition. Whitelist the two
/// spatial inputs at the bridge boundary; a caller cannot smuggle its grading,
/// AI source, repair or managed-raster edits into a Before frame.
pub(crate) fn resolve_composition_before_render(
    catalog: &CatalogHandle,
    runtime_cache_root: &Path,
    photo_id: PhotoId,
    settings: &ffi::FfiEditSettings,
) -> AnyResult<ResolvedRecipeRender> {
    let mut neutral = encode_grade_stack_draft_recipe_v1(GradeStackDraft::default())?;
    neutral.foundation.enabled = settings.foundation.enabled;
    neutral.foundation.optics = settings.foundation.optics.clone();
    neutral.geometry = settings.geometry;
    // This is one complete immutable synthesized Recipe. It deliberately has
    // no base commit and can never inherit the moving working ref.
    resolve_recipe_render(catalog, runtime_cache_root, photo_id, "", &neutral, true)
}

pub(crate) fn resolve_recipe_render(
    catalog: &CatalogHandle,
    runtime_cache_root: &Path,
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
    let foundation = ResolvedFoundationDevelopment::from_snapshot(&snapshot);
    let managed_rasters =
        FilesystemManagedRasterMaskResolver::open_for_runtime_cache(runtime_cache_root)?;
    Ok(ResolvedRecipeRender {
        plan: compile_recipe_render_plan_with_managed_rasters(&snapshot, &managed_rasters)?,
        snapshot_digest,
        foundation,
    })
}

#[cfg(test)]
mod tests;

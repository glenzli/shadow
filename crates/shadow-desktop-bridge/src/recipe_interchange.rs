//! Versioned Shadow Recipe file interchange.
//!
//! Export freezes the complete current Recipe snapshot, including opaque
//! references already owned by that photo. Import is intentionally a separate
//! projection: until source identity can be proved, only reusable Grade Nodes
//! and portable normalized masks are admitted. Photo-private stages remain on
//! the destination photo, managed rasters are never reused as pixels, shared
//! library links are detached, and path-bound LUTs are removed.

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_domain::{
    MaskDefinition, PhotoCanvasNode, PhotoFoundationNode, RawFoundationDenoise, RecipeCommitId,
    ShadowRecipeDocument,
};

use crate::{
    DesktopSession, ffi,
    recipe_v1::{
        GradeNodeRecipeV1Identity, GradeStackDraft, LutEditParameters,
        decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
        encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
        validate_grade_stack_draft_recipe_v1,
    },
};

impl DesktopSession {
    pub(crate) fn export_shadow_recipe_document(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        label: &str,
    ) -> AnyResult<Vec<u8>> {
        let (photo_id, _source) = self.validated_photo_source(photo_id, source_path)?;
        let base_record = if base_commit_id.is_empty() {
            None
        } else {
            let commit_id = base_commit_id
                .parse::<RecipeCommitId>()
                .with_context(|| format!("parse export base Recipe commit id {base_commit_id}"))?;
            Some(
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| {
                        anyhow!("export base Recipe commit {commit_id} is unavailable")
                    })?,
            )
        };
        let draft = decode_grade_stack_draft_recipe_v1(settings)
            .context("decode current desktop Grade Stack for Shadow Recipe export")?;
        let snapshot = grade_stack_recipe_v1_snapshot(
            &draft,
            base_record.as_ref().map(|record| record.commit.snapshot()),
        )
        .context("materialize Shadow Recipe export snapshot")?;
        let label = (!label.is_empty()).then_some(label);
        ShadowRecipeDocument::new(label, snapshot)
            .context("create Shadow Recipe document")?
            .to_pretty_json()
            .context("encode Shadow Recipe document")
    }
}

pub(crate) fn preview_shadow_recipe_document(
    bytes: &[u8],
) -> AnyResult<ffi::FfiShadowRecipeImportPreview> {
    let document =
        ShadowRecipeDocument::from_json(bytes).context("decode Shadow Recipe document")?;
    let label = document.label().unwrap_or_default().to_owned();
    let mut draft = decode_grade_stack_draft_from_recipe_v1_snapshot(document.snapshot())
        .context("project Shadow Recipe into the current editable Grade Stack")?;
    let counts = make_destination_safe(&mut draft)?;
    validate_grade_stack_draft_recipe_v1(&draft)
        .context("validate destination-safe Shadow Recipe projection")?;
    let portable_settings = encode_grade_stack_draft_recipe_v1(draft)
        .context("encode destination-safe Shadow Recipe projection")?;

    Ok(ffi::FfiShadowRecipeImportPreview {
        label,
        portable_settings,
        grade_node_count: counts.grade_node_count,
        portable_mask_count: counts.portable_mask_count,
        managed_mask_node_count: counts.managed_mask_node_count,
        semantic_mask_intent_count: counts.semantic_mask_intent_count,
        detached_shared_node_count: counts.detached_shared_node_count,
        removed_lut_count: counts.removed_lut_count,
        excluded_retouch_region_count: counts.excluded_retouch_region_count,
        excluded_completion_region_count: counts.excluded_completion_region_count,
        excluded_liquify_stroke_count: counts.excluded_liquify_stroke_count,
        foundation_omitted: counts.foundation_omitted,
        raw_denoise_omitted: counts.raw_denoise_omitted,
        canvas_omitted: counts.canvas_omitted,
    })
}

#[derive(Debug, Default, Eq, PartialEq)]
struct ImportCounts {
    grade_node_count: u32,
    portable_mask_count: u32,
    managed_mask_node_count: u32,
    semantic_mask_intent_count: u32,
    detached_shared_node_count: u32,
    removed_lut_count: u32,
    excluded_retouch_region_count: u32,
    excluded_completion_region_count: u32,
    excluded_liquify_stroke_count: u32,
    foundation_omitted: bool,
    raw_denoise_omitted: bool,
    canvas_omitted: bool,
}

fn make_destination_safe(draft: &mut GradeStackDraft) -> AnyResult<ImportCounts> {
    let mut counts = ImportCounts {
        grade_node_count: bounded_count(draft.grade_nodes.len(), "Grade Nodes")?,
        excluded_retouch_region_count: bounded_count(
            draft.retouch_spots.len() + draft.retouch_strokes.len(),
            "repair regions",
        )?,
        excluded_completion_region_count: bounded_count(
            draft.image_completions.len(),
            "completion regions",
        )?,
        excluded_liquify_stroke_count: bounded_count(
            draft
                .liquify
                .as_ref()
                .map_or(0, |node| node.strokes().len()),
            "Liquify strokes",
        )?,
        foundation_omitted: draft.foundation != PhotoFoundationNode::default(),
        raw_denoise_omitted: draft.raw_ai_denoise != RawFoundationDenoise::default(),
        canvas_omitted: draft.canvas != PhotoCanvasNode::identity(),
        ..ImportCounts::default()
    };

    // These complete-source and spatial stages are preserved in the exported
    // document, but never become the destination photo's state through the
    // reusable import path.
    draft.raw_ai_denoise = RawFoundationDenoise::default();
    draft.foundation = PhotoFoundationNode::default();
    draft.retouch_spots.clear();
    draft.retouch_strokes.clear();
    draft.retouch_enabled = true;
    draft.image_completions.clear();
    draft.image_completion_enabled = true;
    draft.liquify = None;
    draft.canvas = PhotoCanvasNode::identity();

    for node in &mut draft.grade_nodes {
        // Imported instances are independent destination nodes. Their graph
        // values remain exact, while every layer/render-op identity is fresh.
        node.recipe_v1_identity = GradeNodeRecipeV1Identity::new();
        if node.shared.take().is_some() {
            counts.detached_shared_node_count += 1;
        }

        if let Some(settings) = node.preserved_managed_raster.take() {
            counts.managed_mask_node_count += 1;
            if settings.semantic_intent.is_some() {
                counts.semantic_mask_intent_count += 1;
            }
            // Applying the adjustment globally would be materially wrong.
            // Keep the authored controls visible, but bypass the node until a
            // destination-local mask can be created or adaptively re-solved.
            node.enabled = false;
        } else if matches!(node.local_mask, Some(MaskDefinition::ManagedRaster { .. })) {
            counts.managed_mask_node_count += 1;
            node.local_mask = None;
            node.enabled = false;
        } else if node.local_mask.is_some() {
            counts.portable_mask_count += 1;
        }

        if !node.fine.lut.resource_id.is_empty()
            || !node.fine.lut.title.is_empty()
            || !node.fine.lut.managed_path.is_empty()
        {
            node.fine.lut = LutEditParameters::default();
            counts.removed_lut_count += 1;
        }
    }

    Ok(counts)
}

fn bounded_count(value: usize, label: &str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("Shadow Recipe {label} count exceeds u32"))
}

#[cfg(test)]
mod tests;

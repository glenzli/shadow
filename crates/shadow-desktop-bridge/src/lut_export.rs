//! Explicit lossy projection of a captured Grade Stack into a color-only LUT.
//! The plan contains values, never a live Catalog reference or source pixels.

use anyhow::{Context, Result};
use shadow_bridge::{
    AdjustmentDetailEffectsPass, AdjustmentRenderOperation, LutBakeCancellation,
    bake_adjustment_lut,
};
use shadow_domain::{
    PhotoCanvasNode, PhotoFoundationNode, RawFoundationDenoise, UnitInterval,
    canonical_recipe_snapshot_digest,
};

use crate::{
    ffi,
    recipe_lut_resources::collect_lut_resources,
    recipe_v1::{
        GradeStackDraft, compile_recipe_render_plan, decode_grade_stack_draft_recipe_v1,
        grade_stack_recipe_v1_snapshot,
    },
};

#[derive(Debug)]
pub(crate) struct LutExportCancellation(LutBakeCancellation);

#[derive(Debug)]
pub(crate) struct LutExportPlan {
    projected: GradeStackDraft,
    included_nodes: Vec<String>,
    omissions: Vec<(String, &'static str)>,
}

pub(crate) fn prepare_lut_export(
    settings: &ffi::FfiEditSettings,
    selected_node_id: &str,
) -> Result<Box<LutExportPlan>> {
    let source =
        decode_grade_stack_draft_recipe_v1(settings).context("decode LUT export snapshot")?;
    Ok(Box::new(project(source, selected_node_id)?))
}

fn project(source: GradeStackDraft, selected_node_id: &str) -> Result<LutExportPlan> {
    let mut omissions = Vec::new();
    for (present, category) in [
        (
            source.foundation != PhotoFoundationNode::default(),
            "foundation",
        ),
        (
            source.raw_ai_denoise != RawFoundationDenoise::default(),
            "rawDenoise",
        ),
        (
            !source.retouch_spots.is_empty() || !source.retouch_strokes.is_empty(),
            "repair",
        ),
        (!source.image_completions.is_empty(), "completion"),
        (source.liquify.is_some(), "liquify"),
        (!source.paint_layers.is_empty(), "paint layers"),
        (source.canvas != PhotoCanvasNode::identity(), "canvas"),
    ] {
        if present {
            omissions.push((String::new(), category));
        }
    }
    if !selected_node_id.is_empty()
        && !source
            .grade_nodes
            .iter()
            .any(|node| node.recipe_v1_identity.grade_node_id.to_string() == selected_node_id)
    {
        anyhow::bail!("the selected Grade Node is unavailable");
    }
    let mut nodes = Vec::new();
    let mut included_nodes = Vec::new();
    for mut node in source.grade_nodes {
        if !selected_node_id.is_empty()
            && node.recipe_v1_identity.grade_node_id.to_string() != selected_node_id
        {
            continue;
        }
        let mut omit = |category| omissions.push((node.label.clone(), category));
        if !node.enabled || node.opacity == UnitInterval::ZERO {
            omit("bypassed");
            continue;
        }
        if node.local_mask.is_some()
            || node.composite_mask.is_some()
            || node.preserved_managed_raster.is_some()
        {
            omit("maskedNode");
            continue;
        }
        if node.fine.selective_tone != shadow_bridge::SelectiveToneParameters::default() {
            omit("regionalTone");
        }
        let detail = &node.fine.sharpen;
        if [
            detail.amount,
            detail.clarity,
            detail.texture,
            detail.local_contrast,
            detail.denoise_luminance,
            detail.denoise_color,
            detail.dehaze,
            detail.defringe_purple_amount,
            detail.defringe_green_amount,
        ]
        .iter()
        .any(|value| *value != 0.0)
        {
            omit("technicalDetail");
        }
        if detail.grain_amount != 0.0 || detail.vignette_amount != 0.0 {
            omit("finishingEffects");
        }
        // These spatial controls share the native ColorGrading pass with
        // color wheels. Remove their footprint before sampling isolated RGBs.
        node.fine.sharpen.clarity = 0.0;
        node.fine.sharpen.texture = 0.0;
        node.fine.sharpen.local_contrast = 0.0;
        node.shared = None;
        included_nodes.push(node.label.clone());
        nodes.push(node);
    }
    Ok(LutExportPlan {
        projected: GradeStackDraft {
            grade_nodes: nodes,
            ..GradeStackDraft::default()
        },
        included_nodes,
        omissions,
    })
}

pub(crate) fn lut_export_preview(plan: &LutExportPlan) -> ffi::FfiLutExportPreview {
    ffi::FfiLutExportPreview {
        included_nodes: plan.included_nodes.clone(),
        omissions: plan
            .omissions
            .iter()
            .map(|(label, reason)| ffi::FfiLutExportOmission {
                node_label: label.clone(),
                reason: (*reason).to_owned(),
            })
            .collect(),
        can_bake: !plan.included_nodes.is_empty(),
    }
}

pub(crate) fn new_lut_export_cancellation() -> Result<Box<LutExportCancellation>> {
    Ok(Box::new(LutExportCancellation(LutBakeCancellation::new()?)))
}

pub(crate) fn cancel_lut_export(cancellation: &LutExportCancellation) {
    cancellation.0.cancel();
}

pub(crate) fn bake_lut_export(
    plan: &LutExportPlan,
    size: u16,
    cancellation: &LutExportCancellation,
) -> Result<ffi::FfiLutExportResult> {
    if plan.included_nodes.is_empty() {
        anyhow::bail!("no unmasked enabled Grade Nodes can be baked");
    }
    collect_lut_resources(&plan.projected).context("verify LUT dependencies before baking")?;
    let snapshot = grade_stack_recipe_v1_snapshot(&plan.projected, None)?;
    let mut render_plan = compile_recipe_render_plan(&snapshot)?;
    render_plan.nodes.retain(|node| match &node.operation {
        AdjustmentRenderOperation::SelectiveTone { .. } => false,
        AdjustmentRenderOperation::Sharpen { pass, .. } => {
            *pass == AdjustmentDetailEffectsPass::ColorGrading
        }
        _ => true,
    });
    let baked = bake_adjustment_lut(&render_plan, size, &cancellation.0)?;
    let digest = canonical_recipe_snapshot_digest(&snapshot)?;
    let digest_hex: String = digest.iter().map(|byte| format!("{byte:02x}")).collect();
    let mut document = format!(
        "# Source grading snapshot BLAKE3: {digest_hex}\n# Omitted items: {}\n",
        plan.omissions.len()
    )
    .into_bytes();
    for (_, category) in &plan.omissions {
        document.extend_from_slice(format!("# Omitted: {category}\n").as_bytes());
    }
    document.extend_from_slice(&baked.document);
    Ok(ffi::FfiLutExportResult {
        document,
        maximum_absolute_error: baked.maximum_absolute_error,
        root_mean_square_error: baked.root_mean_square_error,
        probe_count: baked.probe_count,
    })
}

#[cfg(test)]
mod tests;

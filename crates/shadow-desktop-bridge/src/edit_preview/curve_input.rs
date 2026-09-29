//! Exact pre-curve sampling. This read-only tool never prepares a missing source.
use super::WarmEditPreviewSourceRequest;
use crate::{
    DesktopSession, ffi,
    preview_cache_identity::current_source_environment_cache_identity,
    preview_render_registry::{PreviewAdmission, PreviewTerminalClaim},
    raw_foundation_render_source::raw_foundation_ready_for_render,
    recipe_v1::{
        recipe_v1_oklab_lightness_tone_curve_render_op_id, recipe_v1_rgb_tone_curves_render_op_id,
        resolve_recipe_render,
    },
    session_photo_source::catalog_native_path,
};
use anyhow::{Result, anyhow, ensure};
use shadow_bridge::{
    AdjustmentRenderOperation as Op, AdjustmentRenderPlan, RgbToneCurves, photo_provider_version,
};
use shadow_domain::LayerInstanceId;

fn curve_prefix(
    mut plan: AdjustmentRenderPlan,
    node: &ffi::FfiGradeNode,
    channel: u8,
) -> Result<AdjustmentRenderPlan> {
    ensure!(channel <= 4 && node.enabled, "curve target is unavailable");
    let id: LayerInstanceId = node.grade_node_id.parse()?;
    let luma = format!(
        "{id}/{}",
        recipe_v1_oklab_lightness_tone_curve_render_op_id(id)
    );
    let rgb = format!("{id}/{}", recipe_v1_rgb_tone_curves_render_op_id(id));
    let detail = format!("{id}/{}", node.sharpen_render_op_id);
    let anchor = plan
        .nodes
        .iter()
        .position(|n| n.node_id == detail)
        .ok_or_else(|| anyhow!("curve target is missing its technical-detail boundary"))?;
    let end = plan
        .nodes
        .iter()
        .position(|n| n.node_id == rgb)
        .unwrap_or(anchor);
    let end = if channel == 0 {
        plan.nodes
            .iter()
            .position(|n| n.node_id == luma)
            .unwrap_or(end)
    } else {
        end
    };
    let keep_master = channel >= 2 && plan.nodes.get(end).is_some_and(|n| n.node_id == rgb);
    plan.nodes.truncate(end + usize::from(keep_master));
    if keep_master {
        if let Op::RgbToneCurves { curves } = &mut plan
            .nodes
            .last_mut()
            .expect("nonempty curve prefix")
            .operation
        {
            for curve in &mut curves.channels[1..] {
                *curve = RgbToneCurves::default().channels[0].clone();
            }
        }
    }
    let mut open = None;
    for (index, n) in plan.nodes.iter().enumerate() {
        match n.operation {
            Op::LocalMaskLayerStart { .. } => open = Some(index),
            Op::LocalMaskLayerEnd => open = None,
            _ => {}
        }
    }
    // Sample the selected curve's input, before its own mask/opacity blend.
    if let Some(index) = open {
        plan.nodes[index].operation = Op::LocalMaskLayerStart {
            opacity: 1.0,
            mask: None,
        };
        let mut closing = plan.nodes[index].clone();
        closing.node_id = "targeted-curve-input:end".into();
        closing.operation = Op::LocalMaskLayerEnd;
        plan.nodes.push(closing);
    }
    plan.validate()?;
    Ok(plan)
}

impl DesktopSession {
    pub(crate) fn curve_input_map(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
        grade_node_index: u32,
        channel: u8,
    ) -> Result<ffi::FfiCurveInputMap> {
        let empty = || ffi::FfiCurveInputMap {
            width: 0,
            height: 0,
            values: vec![],
        };
        let result = (|| {
            if self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|e| anyhow!("curve input admission: {e:?}"))?
                == PreviewAdmission::Cancelled
            {
                return Ok(empty());
            }
            ensure!(
                request.use_working_recipe
                    && request.policy == ffi::FfiEditPreviewPolicy::Interactive,
                "curve input requires the working recipe"
            );
            let cancellation = self
                .edit_preview_render_tokens
                .cancellation(request.render_token)
                .map_err(|e| anyhow!("curve input cancellation: {e:?}"))?;
            let foundation_cancellation = self
                .edit_preview_render_tokens
                .foundation_cancellation(request.render_token)
                .map_err(|e| anyhow!("curve input source cancellation: {e:?}"))?;
            let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
            let recipe = resolve_recipe_render(
                &self.catalog,
                &self.cache_root,
                photo_id,
                &request.base_commit_id,
                &request.settings,
                true,
            )?;
            let node = request
                .settings
                .grade_nodes
                .get(usize::try_from(grade_node_index)?)
                .ok_or_else(|| anyhow!("invalid curve target"))?;
            let plan = curve_prefix(recipe.plan, node, channel)?;
            let identity = current_source_environment_cache_identity(&photo_provider_version());
            let native_path = catalog_native_path(&source)?;
            let foundation = raw_foundation_ready_for_render(
                &self.raw_foundations,
                &self.raw_foundation_runtime,
                &native_path,
                source.source,
                recipe.foundation.raw_ai_denoise(),
                &foundation_cancellation,
            )?;
            let session =
                self.warm_edit_preview_sessions
                    .get_existing(&WarmEditPreviewSourceRequest {
                        runtime_cache_root: &self.cache_root,
                        source: &source,
                        max_edge: request.max_edge,
                        raw_development_plan: recipe.foundation.preview_plan(),
                        optics: recipe.foundation.optics(),
                        source_environment_cache_identity: &identity,
                        raw_foundation: foundation.as_ref(),
                        interactive_timing_token: None,
                    })?;
            let Some(session) = session else {
                return Ok(empty());
            };
            let map = session.curve_input_map(&plan, channel, &cancellation)?;
            Ok(ffi::FfiCurveInputMap {
                width: map.width,
                height: map.height,
                values: map.values,
            })
        })();
        match self
            .edit_preview_render_tokens
            .claim_terminal(request.render_token)
            .map_err(|e| anyhow!("curve input terminal: {e:?}"))?
        {
            PreviewTerminalClaim::Cancelled => Ok(empty()),
            PreviewTerminalClaim::Completed => result,
        }
    }
}

#[cfg(test)]
mod tests;

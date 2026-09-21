//! Canonical Recipe v1 Grade Node graph shape and ordered operation projection.
//!
//! This owner validates the persisted linear-chain contract and maps it to
//! responsibility-named operation slots. Snapshot encoding, reverse decoding,
//! render-plan compilation, and version summaries all consume this one
//! structural interpretation.

use std::collections::{HashMap, HashSet};

use anyhow::{Result as AnyResult, anyhow, bail};
use shadow_bridge::MAX_ADJUSTMENT_RENDER_NODES;
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, COLOR_GRADING_IMPLEMENTATION_VERSION, COLOR_GRADING_OPERATION_ID,
    CONTRAST_OPERATION_ID, EXPOSURE_OPERATION_ID, FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
    FINISHING_EFFECTS_OPERATION_ID, LUT_3D_OPERATION_ID, OKLAB_COLOR_WARPER_OPERATION_ID,
    OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID, RGB_WHITE_BALANCE_OPERATION_ID,
    SATURATION_OPERATION_ID, TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
    TECHNICAL_DETAIL_OPERATION_ID,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, ImageDomain, LayerInstance, NodeInput, PortType,
    ProcessingStage,
};

#[cfg(test)]
use shadow_domain::{LayerInstanceId, NodeId, RecipeSnapshot};

use super::{
    validate_recipe_detail_effects_render_op, validate_recipe_oklab_color_warper_render_op,
    validate_recipe_oklab_lightness_tone_curve_render_op,
    validate_recipe_perceptual_color_render_op, validate_recipe_selective_tone_render_op,
    validate_recipe_v1_render_op,
};

#[cfg(test)]
use super::basic_parameters_from_snapshot;

pub(crate) fn ordered_layer_nodes(layer: &LayerInstance) -> AnyResult<Vec<&AdjustmentNode>> {
    if layer.scope() != AdjustmentScope::Photo || layer.blend_mode() != BlendMode::Normal {
        bail!("Recipe render compiler does not support this layer scope or blend mode");
    }
    let graph = layer.content().graph();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if graph.schema_version() != BASIC_GRAPH_SCHEMA_VERSION
        || graph.input_types() != [rgb]
        || graph.output_type() != Some(rgb)
    {
        bail!("Recipe render compiler received an unsupported graph contract");
    }
    if graph.nodes().is_empty() || graph.nodes().len() > MAX_ADJUSTMENT_RENDER_NODES {
        bail!("Recipe render compiler supports 1 through 256 executable nodes");
    }

    let mut reverse = Vec::with_capacity(graph.nodes().len());
    let mut visited = HashSet::with_capacity(graph.nodes().len());
    let nodes_by_id = graph
        .nodes()
        .iter()
        .map(|node| (node.id(), node))
        .collect::<HashMap<_, _>>();
    let mut current = graph.output_node();
    loop {
        if !visited.insert(current) {
            bail!("Recipe render compiler encountered a dependency cycle at node {current}");
        }
        let node = nodes_by_id
            .get(&current)
            .copied()
            .ok_or_else(|| anyhow!("Recipe output path references missing node {current}"))?;
        reverse.push(node);
        match node.inputs() {
            [NodeInput::GraphInput { index: 0 }] => break,
            [NodeInput::Node { node_id }] => current = *node_id,
            _ => bail!(
                "Recipe node {} is not part of the supported single-input linear chain",
                node.id()
            ),
        }
    }
    if reverse.len() != graph.nodes().len() {
        bail!("Recipe render compiler rejects branches or nodes outside the output chain");
    }
    reverse.reverse();
    Ok(reverse)
}

pub(crate) struct GradeNodeRecipeV1RenderOps<'a> {
    #[cfg(test)]
    pub(crate) layer: &'a LayerInstance,
    pub(crate) exposure: &'a AdjustmentNode,
    pub(crate) contrast: &'a AdjustmentNode,
    pub(crate) selective_tone: &'a AdjustmentNode,
    pub(crate) rgb_tone_curves: Option<&'a AdjustmentNode>,
    pub(crate) oklab_lightness_curve: Option<&'a AdjustmentNode>,
    pub(crate) white_balance: &'a AdjustmentNode,
    pub(crate) saturation: &'a AdjustmentNode,
    pub(crate) perceptual_color: &'a AdjustmentNode,
    pub(crate) oklab_color_warper: Option<&'a AdjustmentNode>,
    pub(crate) technical_detail: &'a AdjustmentNode,
    pub(crate) color_grading: &'a AdjustmentNode,
    pub(crate) lut: &'a AdjustmentNode,
    pub(crate) finishing_effects: &'a AdjustmentNode,
}

impl GradeNodeRecipeV1RenderOps<'_> {
    pub(super) fn ordered(&self) -> Vec<&AdjustmentNode> {
        let mut nodes = vec![
            self.white_balance,
            self.exposure,
            self.contrast,
            self.selective_tone,
            self.saturation,
            self.perceptual_color,
        ];
        if let Some(oklab_color_warper) = self.oklab_color_warper {
            nodes.push(oklab_color_warper);
        }
        if let Some(oklab_lightness_curve) = self.oklab_lightness_curve {
            nodes.push(oklab_lightness_curve);
        }
        if let Some(curves) = self.rgb_tone_curves {
            nodes.push(curves);
        }
        nodes.extend([
            self.technical_detail,
            self.color_grading,
            self.lut,
            self.finishing_effects,
        ]);
        nodes
    }
}

#[cfg(test)]
pub(crate) fn single_grade_node_recipe_v1_render_ops(
    snapshot: &RecipeSnapshot,
) -> AnyResult<GradeNodeRecipeV1RenderOps<'_>> {
    let [layer] = snapshot.layers() else {
        bail!("Basic Recipe helper requires exactly one adjustment layer");
    };
    grade_node_recipe_v1_render_ops(layer)
}

#[allow(clippy::too_many_lines)] // The canonical chain contract is intentionally explicit.
pub(crate) fn grade_node_recipe_v1_render_ops(
    layer: &LayerInstance,
) -> AnyResult<GradeNodeRecipeV1RenderOps<'_>> {
    let ordered = ordered_layer_nodes(layer)?;
    if !(10..=13).contains(&ordered.len()) {
        bail!("working Recipe is not the current complete Grade Node shape");
    }
    let white_balance = ordered[0];
    let exposure = ordered[1];
    let contrast = ordered[2];
    let selective_tone = ordered[3];
    let saturation = ordered[4];
    let perceptual_color = ordered[5];
    let mut cursor = 6_usize;
    let mut oklab_color_warper = None;
    if ordered.get(cursor).is_some_and(|node| {
        node.operation().operation_id().as_str() == OKLAB_COLOR_WARPER_OPERATION_ID
    }) {
        oklab_color_warper = Some(ordered[cursor]);
        cursor += 1;
    }
    let mut oklab_lightness_curve = None;
    if ordered.get(cursor).is_some_and(|node| {
        node.operation().operation_id().as_str() == OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID
    }) {
        oklab_lightness_curve = Some(ordered[cursor]);
        cursor += 1;
    }
    let mut rgb_tone_curves = None;
    if ordered.get(cursor).is_some_and(|node| {
        node.operation().operation_id().as_str()
            == shadow_domain::operation::RGB_TONE_CURVES_OPERATION_ID
    }) {
        rgb_tone_curves = Some(ordered[cursor]);
        cursor += 1;
    }
    if ordered.len() != cursor + 4 {
        bail!("working Recipe has an unsupported curve ordering");
    }
    let technical_detail = ordered[cursor];
    let color_grading = ordered[cursor + 1];
    let lut = ordered[cursor + 2];
    let finishing_effects = ordered[cursor + 3];
    validate_recipe_v1_render_op(
        white_balance,
        RGB_WHITE_BALANCE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::GraphInput { index: 0 },
    )?;
    validate_recipe_v1_render_op(
        exposure,
        EXPOSURE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::Node {
            node_id: white_balance.id(),
        },
    )?;
    validate_recipe_v1_render_op(
        contrast,
        CONTRAST_OPERATION_ID,
        ProcessingStage::ToneAndLocalContrast,
        NodeInput::Node {
            node_id: exposure.id(),
        },
    )?;
    validate_recipe_selective_tone_render_op(
        selective_tone,
        NodeInput::Node {
            node_id: contrast.id(),
        },
    )?;
    validate_recipe_v1_render_op(
        saturation,
        SATURATION_OPERATION_ID,
        ProcessingStage::ToneAndLocalContrast,
        NodeInput::Node {
            node_id: selective_tone.id(),
        },
    )?;
    validate_recipe_perceptual_color_render_op(
        perceptual_color,
        NodeInput::Node {
            node_id: saturation.id(),
        },
    )?;
    let mut technical_input = perceptual_color.id();
    if let Some(oklab_color_warper) = oklab_color_warper {
        validate_recipe_oklab_color_warper_render_op(
            oklab_color_warper,
            NodeInput::Node {
                node_id: technical_input,
            },
        )?;
        technical_input = oklab_color_warper.id();
    }
    if let Some(oklab_lightness_curve) = oklab_lightness_curve {
        validate_recipe_oklab_lightness_tone_curve_render_op(
            oklab_lightness_curve,
            NodeInput::Node {
                node_id: technical_input,
            },
        )?;
        technical_input = oklab_lightness_curve.id();
    }
    if let Some(curves) = rgb_tone_curves {
        super::rgb_tone_curves::validate_rgb_tone_curves_render_op(
            curves,
            NodeInput::Node {
                node_id: technical_input,
            },
        )?;
        technical_input = curves.id();
    }
    validate_recipe_detail_effects_render_op(
        technical_detail,
        TECHNICAL_DETAIL_OPERATION_ID,
        TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
        ProcessingStage::TechnicalDetail,
        NodeInput::Node {
            node_id: technical_input,
        },
    )?;
    validate_recipe_detail_effects_render_op(
        color_grading,
        COLOR_GRADING_OPERATION_ID,
        COLOR_GRADING_IMPLEMENTATION_VERSION,
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: technical_detail.id(),
        },
    )?;
    validate_recipe_v1_render_op(
        lut,
        LUT_3D_OPERATION_ID,
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: color_grading.id(),
        },
    )?;
    validate_recipe_detail_effects_render_op(
        finishing_effects,
        FINISHING_EFFECTS_OPERATION_ID,
        FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
        ProcessingStage::FinishingEffects,
        NodeInput::Node { node_id: lut.id() },
    )?;
    Ok(GradeNodeRecipeV1RenderOps {
        #[cfg(test)]
        layer,
        exposure,
        contrast,
        selective_tone,
        rgb_tone_curves,
        oklab_lightness_curve,
        white_balance,
        saturation,
        perceptual_color,
        oklab_color_warper,
        technical_detail,
        color_grading,
        lut,
        finishing_effects,
    })
}

#[cfg(test)]
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct GradeNodeRecipeV1TestIdentity {
    pub(crate) grade_node_id: LayerInstanceId,
    pub(crate) render_op_ids: [NodeId; 10],
}

#[cfg(test)]
pub(crate) fn single_grade_node_recipe_v1_identity(
    snapshot: &RecipeSnapshot,
) -> AnyResult<Option<GradeNodeRecipeV1TestIdentity>> {
    if snapshot.layers().is_empty() {
        return Ok(None);
    }
    let [layer] = snapshot.layers() else {
        bail!("Basic Recipe identity helper requires exactly one layer");
    };
    basic_parameters_from_snapshot(snapshot)?;
    let nodes = grade_node_recipe_v1_render_ops(layer)?;
    Ok(Some(GradeNodeRecipeV1TestIdentity {
        grade_node_id: nodes.layer.id(),
        render_op_ids: [
            nodes.exposure.id(),
            nodes.contrast.id(),
            nodes.selective_tone.id(),
            nodes.white_balance.id(),
            nodes.saturation.id(),
            nodes.perceptual_color.id(),
            nodes.technical_detail.id(),
            nodes.color_grading.id(),
            nodes.lut.id(),
            nodes.finishing_effects.id(),
        ],
    }))
}

#[cfg(test)]
mod tests;

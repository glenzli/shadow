//! Encoding editable Grade Stack state into immutable Recipe v1 snapshots and render operations.

use std::collections::BTreeMap;

use anyhow::{Result as AnyResult, bail};
use shadow_bridge::{
    ColorRangeParameters, MAX_POINT_COLOR_RANGES, OklabColorWarperParameters,
    OklabLightnessToneCurve, PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION, PerceptualColorParameters,
    SelectiveToneParameters, SharpenParameters, ToneCurvePoint,
};
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, BLACKS_PARAMETER_KEY, COLOR_GRADING_IMPLEMENTATION_VERSION,
    COLOR_GRADING_OPERATION_ID, COLOR_MIXER_HUE_PARAMETER_KEY, COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
    COLOR_MIXER_SATURATION_PARAMETER_KEY, COLOR_RANGE_CENTER_PARAMETER_KEY,
    COLOR_RANGE_ENABLED_PARAMETER_KEY, COLOR_RANGE_HUE_PARAMETER_KEY,
    COLOR_RANGE_LIGHTNESS_PARAMETER_KEY, COLOR_RANGE_SATURATION_PARAMETER_KEY,
    COLOR_RANGE_SOFTNESS_PARAMETER_KEY, COLOR_RANGE_WIDTH_PARAMETER_KEY,
    CONTRAST_FACTOR_PARAMETER_KEY, CONTRAST_OPERATION_ID, CONTRAST_PIVOT_PARAMETER_KEY,
    CPU_REFERENCE_IMPLEMENTATION_VERSION, CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
    DETAIL_EFFECTS_PARAMETERS_KEY, EXPOSURE_OPERATION_ID, EXPOSURE_STOPS_PARAMETER_KEY,
    FINISHING_EFFECTS_IMPLEMENTATION_VERSION, FINISHING_EFFECTS_OPERATION_ID,
    GLOBAL_A_BALANCE_PARAMETER_KEY, GLOBAL_B_BALANCE_PARAMETER_KEY, HIGHLIGHTS_PARAMETER_KEY,
    LUT_3D_OPERATION_ID, LUT_INTENSITY_PARAMETER_KEY, LUT_MANAGED_PATH_PARAMETER_KEY,
    LUT_RESOURCE_ID_PARAMETER_KEY, LUT_TITLE_PARAMETER_KEY,
    OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY, OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
    OKLAB_COLOR_WARPER_OPERATION_ID, OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
    OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID, OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY, PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
    PERCEPTUAL_COLOR_OPERATION_ID, POINT_COLOR_RANGES_PARAMETER_KEY,
    RGB_WHITE_BALANCE_OPERATION_ID, SATURATION_FACTOR_PARAMETER_KEY, SATURATION_OPERATION_ID,
    SELECTIVE_COLOR_CMYK_PARAMETER_KEY, SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
    SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY, SELECTIVE_TONE_IMPLEMENTATION_VERSION,
    SELECTIVE_TONE_OPERATION_ID, SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION, SHADOWS_PARAMETER_KEY,
    SHARPEN_AMOUNT_PARAMETER_KEY, SHARPEN_MASKING_PARAMETER_KEY, SHARPEN_RADIUS_PARAMETER_KEY,
    SHARPEN_THRESHOLD_PARAMETER_KEY, TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
    TECHNICAL_DETAIL_OPERATION_ID, TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
    VIBRANCE_PARAMETER_KEY, WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
    WHITE_BALANCE_TINT_PARAMETER_KEY, WHITES_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditGraph,
    FiniteF64, ImageDomain, LayerContent, LayerInstance, LayerRevisionSelector, MaskRevision,
    NodeId, NodeInput, OperationDescriptor, OperationId, ParameterBlock, ParameterKey,
    ParameterValue, PortType, ProcessingStage, RecipeInputSettings, RecipeSnapshot, UnitInterval,
};

use super::{
    CONTRAST_PIVOT, GradeNodeDraft, GradeStackDraft, LutEditParameters,
    oklab_color_warper_ffi_values, ordered_layer_nodes, recipe_v1_local_mask_revision,
    validate_grade_stack_draft_against_recipe_v1_template, validate_grade_stack_draft_recipe_v1,
    validate_recipe_detail_effects_render_op, validate_recipe_oklab_color_warper_render_op,
    validate_recipe_oklab_lightness_tone_curve_render_op,
    validate_recipe_perceptual_color_render_op, validate_recipe_selective_tone_render_op,
    validate_recipe_v1_render_op, validate_tone_curve,
};

#[cfg(test)]
use super::{basic_parameters_from_snapshot, decode_grade_stack_draft_from_recipe_v1_snapshot};
#[cfg(test)]
use shadow_bridge::BasicEditParameters;
#[cfg(test)]
use shadow_domain::LayerInstanceId;

#[cfg(test)]
pub(crate) fn basic_recipe_snapshot(
    parameters: BasicEditParameters,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    let mut grade_stack = template
        .map(decode_grade_stack_draft_from_recipe_v1_snapshot)
        .transpose()?
        .unwrap_or_default();
    grade_stack.basic = parameters;
    grade_stack_recipe_v1_snapshot(&grade_stack, template)
}

pub(crate) fn grade_stack_recipe_v1_snapshot(
    grade_stack: &GradeStackDraft,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    validate_grade_stack_draft_recipe_v1(grade_stack)?;
    if let Some(template) = template {
        validate_grade_stack_draft_against_recipe_v1_template(grade_stack, template)?;
    }
    let recipe_v1_layers = grade_stack
        .grade_nodes
        .iter()
        .map(encode_grade_node_as_recipe_v1_layer)
        .collect::<AnyResult<Vec<_>>>()?;
    let recipe_v1_masks = grade_stack
        .grade_nodes
        .iter()
        .filter_map(|grade_node| {
            grade_node.local_mask.as_ref().map(|definition| {
                recipe_v1_local_mask_revision(
                    grade_node.recipe_v1_identity.grade_node_id,
                    definition,
                )
            })
        })
        .collect::<AnyResult<Vec<_>>>()?;
    RecipeSnapshot::new_with_input_settings_masks_retouch_strokes_and_geometry(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::new(grade_stack.optics.clone()),
        recipe_v1_masks,
        grade_stack.retouch_spots.clone(),
        grade_stack.retouch_strokes.clone(),
        grade_stack.geometry,
        recipe_v1_layers,
    )
    .map_err(Into::into)
}

#[allow(clippy::too_many_lines)] // The canonical persisted graph is clearest as one explicit chain.
pub(crate) fn encode_grade_node_as_recipe_v1_layer(
    grade_node: &GradeNodeDraft,
) -> AnyResult<LayerInstance> {
    let parameters = grade_node.basic;
    let fine = &grade_node.fine;
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let identity = &grade_node.recipe_v1_identity;
    let local_mask = grade_node
        .local_mask
        .as_ref()
        .map(|definition| recipe_v1_local_mask_revision(identity.grade_node_id, definition))
        .transpose()?;
    let exposure_id = identity.exposure_render_op_id;
    let contrast_id = identity.contrast_render_op_id;
    let selective_tone_id = identity.selective_tone_render_op_id;
    let white_balance_id = identity.white_balance_render_op_id;
    let saturation_id = identity.saturation_render_op_id;
    let perceptual_color_id = identity.perceptual_color_render_op_id;
    let oklab_color_warper_id = identity.oklab_color_warper_render_op_id;
    let oklab_lightness_curve_id = identity.oklab_lightness_curve_render_op_id;
    let lut_id = identity.lut_render_op_id;
    let technical_detail_id = identity.sharpen_render_op_id;
    let color_grading_id = identity.color_grading_render_op_id;
    let finishing_effects_id = identity.finishing_effects_render_op_id;
    let mut nodes = vec![
        // This is a scene-linear, post-demosaic chromatic adaptation rather than sensor-domain
        // white balance. It must still precede exposure and tone mapping: otherwise a white-
        // balance change changes how the perceptual lightness curve and highlight shoulder
        // treat a neutral.
        recipe_v1_render_op(
            white_balance_id,
            RGB_WHITE_BALANCE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            parameter_block([
                (
                    WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(parameters.white_balance_temperature)?),
                ),
                (
                    WHITE_BALANCE_TINT_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(parameters.white_balance_tint)?),
                ),
            ])?,
        )?,
        recipe_v1_render_op(
            exposure_id,
            EXPOSURE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::Node {
                node_id: white_balance_id,
            },
            parameter_block([(
                EXPOSURE_STOPS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.exposure_stops)?),
            )])?,
        )?,
        recipe_v1_render_op(
            contrast_id,
            CONTRAST_OPERATION_ID,
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: exposure_id,
            },
            parameter_block([
                (
                    CONTRAST_FACTOR_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(parameters.contrast_factor)?),
                ),
                (
                    CONTRAST_PIVOT_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(CONTRAST_PIVOT)?),
                ),
            ])?,
        )?,
        recipe_selective_tone_render_op(
            selective_tone_id,
            NodeInput::Node {
                node_id: contrast_id,
            },
            fine.selective_tone,
        )?,
        // Foundational color controls deliberately precede the user curve, so
        // their behavior does not depend on a later tonal remapping. Creative
        // wheels and LUTs remain in the later CreativeColor stage.
        recipe_v1_render_op(
            saturation_id,
            SATURATION_OPERATION_ID,
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: selective_tone_id,
            },
            parameter_block([(
                SATURATION_FACTOR_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.saturation_factor)?),
            )])?,
        )?,
        recipe_perceptual_color_render_op(
            perceptual_color_id,
            NodeInput::Node {
                node_id: saturation_id,
            },
            &fine.perceptual_color,
        )?,
    ];
    // The connected Color Warper mesh comes after the hue-keyed controls: the
    // Color Mixer and Point Color therefore retain their authored source-hue
    // semantics, while the mesh is a separate chroma-field correction.
    let color_warper_input = if is_neutral_oklab_color_warper(&fine.oklab_color_warper) {
        perceptual_color_id
    } else {
        nodes.push(recipe_oklab_color_warper_render_op(
            oklab_color_warper_id,
            NodeInput::Node {
                node_id: perceptual_color_id,
            },
            &fine.oklab_color_warper,
        )?);
        oklab_color_warper_id
    };
    // Perceptual L belongs after the hue/chroma controls, before technical
    // recovery and creative LUTs.
    let perceptual_tone_input = if let Some(curve) = fine.oklab_lightness_curve.as_ref() {
        nodes.push(recipe_oklab_lightness_tone_curve_render_op(
            oklab_lightness_curve_id,
            NodeInput::Node {
                node_id: color_warper_input,
            },
            curve,
        )?);
        oklab_lightness_curve_id
    } else {
        color_warper_input
    };
    // The former monolithic Detail & Effects node is deliberately expanded
    // here, not in the UI: foundational color and the user curve run before
    // technical recovery; color wheels stay in CreativeColor, and physical
    // finishing is last.
    nodes.push(recipe_detail_effects_render_op(
        technical_detail_id,
        TECHNICAL_DETAIL_OPERATION_ID,
        TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
        ProcessingStage::TechnicalDetail,
        NodeInput::Node {
            node_id: perceptual_tone_input,
        },
        &fine.sharpen,
    )?);
    nodes.extend([
        recipe_detail_effects_render_op(
            color_grading_id,
            COLOR_GRADING_OPERATION_ID,
            COLOR_GRADING_IMPLEMENTATION_VERSION,
            ProcessingStage::CreativeColor,
            NodeInput::Node {
                node_id: technical_detail_id,
            },
            &fine.sharpen,
        )?,
        recipe_lut_render_op(
            lut_id,
            NodeInput::Node {
                node_id: color_grading_id,
            },
            &fine.lut,
        )?,
        recipe_detail_effects_render_op(
            finishing_effects_id,
            FINISHING_EFFECTS_OPERATION_ID,
            FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
            ProcessingStage::FinishingEffects,
            NodeInput::Node { node_id: lut_id },
            &fine.sharpen,
        )?,
    ]);
    let graph = EditGraph::new(
        BASIC_GRAPH_SCHEMA_VERSION,
        vec![rgb],
        nodes,
        finishing_effects_id,
    )?;
    let content = match grade_node.shared {
        None => LayerContent::Inline { graph },
        Some(shared) => LayerContent::Shared {
            layer_id: shared.layer_id,
            revision: LayerRevisionSelector::Pinned(shared.revision_id),
            graph,
        },
    };
    LayerInstance::new(
        identity.grade_node_id,
        grade_node.label.clone(),
        AdjustmentScope::Photo,
        content,
        grade_node.enabled,
        UnitInterval::ONE,
        BlendMode::Normal,
        local_mask.as_ref().map(MaskRevision::reference),
    )
    .map_err(Into::into)
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

pub(crate) fn recipe_v1_render_op(
    id: NodeId,
    operation_id: &str,
    stage: ProcessingStage,
    input: NodeInput,
    parameters: ParameterBlock,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(operation_id)?,
        CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
        CPU_REFERENCE_IMPLEMENTATION_VERSION,
        stage,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(id, operation, vec![input], parameters, None).map_err(Into::into)
}

pub(crate) fn recipe_selective_tone_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: SelectiveToneParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(SELECTIVE_TONE_OPERATION_ID)?,
        SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
        SELECTIVE_TONE_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([
            (
                HIGHLIGHTS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.highlights)?),
            ),
            (
                SHADOWS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.shadows)?),
            ),
            (
                WHITES_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.whites)?),
            ),
            (
                BLACKS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.blacks)?),
            ),
        ])?,
        None,
    )
    .map_err(Into::into)
}

pub(crate) fn recipe_oklab_lightness_tone_curve_render_op(
    id: NodeId,
    input: NodeInput,
    curve: &OklabLightnessToneCurve,
) -> AnyResult<AdjustmentNode> {
    validate_tone_curve(&curve.lightness)?;
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID)?,
        OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
        OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([(
            OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY,
            tone_curve_parameter_value(&curve.lightness)?,
        )])?,
        None,
    )
    .map_err(Into::into)
}

pub(crate) fn parameter_block<const N: usize>(
    entries: [(&str, ParameterValue); N],
) -> AnyResult<ParameterBlock> {
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    Ok(ParameterBlock::new(values))
}

pub(crate) fn point_color_ranges_from_vector(
    flattened: &[f64],
) -> AnyResult<Vec<ColorRangeParameters>> {
    if !flattened.len().is_multiple_of(7) {
        bail!("Point Color range storage must contain groups of seven values");
    }
    let ranges = flattened
        .chunks_exact(7)
        .map(|values| {
            let enabled = match values[0].to_bits() {
                bits if bits == 0.0_f64.to_bits() => false,
                bits if bits == 1.0_f64.to_bits() => true,
                _ => bail!("Point Color enabled values must be zero or one"),
            };
            Ok(ColorRangeParameters {
                enabled,
                center_hue_degrees: values[1],
                width_degrees: values[2],
                softness: values[3],
                hue_shift_degrees: values[4],
                saturation: values[5],
                lightness: values[6],
            })
        })
        .collect::<AnyResult<Vec<_>>>()?;
    if ranges.len() + 1 > MAX_POINT_COLOR_RANGES {
        bail!("Point Color supports at most {MAX_POINT_COLOR_RANGES} ordered ranges");
    }
    Ok(ranges)
}

pub(crate) fn point_color_ranges_from_vector_optional(
    flattened: &[f64],
) -> AnyResult<Vec<ColorRangeParameters>> {
    if flattened.is_empty() {
        Ok(Vec::new())
    } else {
        point_color_ranges_from_vector(flattened)
    }
}

pub(crate) fn perceptual_color_parameter_block(
    parameters: &PerceptualColorParameters,
) -> AnyResult<ParameterBlock> {
    let range = parameters.color_range;
    let mut entries = vec![
        (
            GLOBAL_A_BALANCE_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.global_a_balance)?),
        ),
        (
            GLOBAL_B_BALANCE_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.global_b_balance)?),
        ),
        (
            VIBRANCE_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.vibrance)?),
        ),
        (
            COLOR_MIXER_HUE_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .hue_shifts
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
        (
            COLOR_MIXER_SATURATION_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .saturation
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
        (
            COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .lightness
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
        (
            COLOR_RANGE_ENABLED_PARAMETER_KEY,
            ParameterValue::Bool(range.enabled),
        ),
        (
            COLOR_RANGE_CENTER_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.center_hue_degrees)?),
        ),
        (
            COLOR_RANGE_WIDTH_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.width_degrees)?),
        ),
        (
            COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.softness)?),
        ),
        (
            COLOR_RANGE_HUE_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.hue_shift_degrees)?),
        ),
        (
            COLOR_RANGE_SATURATION_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.saturation)?),
        ),
        (
            COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.lightness)?),
        ),
        (
            SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY,
            ParameterValue::Bool(parameters.selective_color_relative),
        ),
        (
            SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(
                parameters.selective_color_lightness_protection,
            )?),
        ),
        (
            SELECTIVE_COLOR_CMYK_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .selective_color_cmyk
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
    ];
    let flattened = parameters
        .additional_color_ranges
        .iter()
        .flat_map(|range| {
            [
                if range.enabled { 1.0 } else { 0.0 },
                range.center_hue_degrees,
                range.width_degrees,
                range.softness,
                range.hue_shift_degrees,
                range.saturation,
                range.lightness,
            ]
        })
        .map(FiniteF64::new)
        .collect::<Result<Vec<_>, _>>()?;
    entries.push((
        POINT_COLOR_RANGES_PARAMETER_KEY,
        ParameterValue::FloatVector(flattened),
    ));
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    Ok(ParameterBlock::new(values))
}

pub(crate) fn recipe_perceptual_color_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: &PerceptualColorParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(PERCEPTUAL_COLOR_OPERATION_ID)?,
        PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION,
        PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        perceptual_color_parameter_block(parameters)?,
        None,
    )
    .map_err(Into::into)
}

pub(crate) fn recipe_oklab_color_warper_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: &OklabColorWarperParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(OKLAB_COLOR_WARPER_OPERATION_ID)?,
        OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
        OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([
            (
                OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY,
                ParameterValue::FloatVector(
                    oklab_color_warper_ffi_values(parameters)
                        .into_iter()
                        .map(FiniteF64::new)
                        .collect::<Result<Vec<_>, _>>()?,
                ),
            ),
            (
                OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.strength)?),
            ),
        ])?,
        None,
    )
    .map_err(Into::into)
}

pub(crate) fn is_neutral_oklab_color_warper(parameters: &OklabColorWarperParameters) -> bool {
    parameters
        .control_points
        .iter()
        .all(|point| point.a_offset == 0.0 && point.b_offset == 0.0)
}

pub(crate) fn recipe_lut_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: &LutEditParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(LUT_3D_OPERATION_ID)?,
        CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
        CPU_REFERENCE_IMPLEMENTATION_VERSION,
        ProcessingStage::CreativeColor,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([
            (
                LUT_RESOURCE_ID_PARAMETER_KEY,
                ParameterValue::Text(parameters.resource_id.clone()),
            ),
            (
                LUT_TITLE_PARAMETER_KEY,
                ParameterValue::Text(parameters.title.clone()),
            ),
            (
                LUT_MANAGED_PATH_PARAMETER_KEY,
                ParameterValue::Text(parameters.managed_path.clone()),
            ),
            (
                LUT_INTENSITY_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.intensity)?),
            ),
        ])?,
        None,
    )
    .map_err(Into::into)
}

pub(crate) fn detail_effect_values(parameters: &SharpenParameters) -> [f64; 33] {
    [
        parameters.clarity,
        parameters.texture,
        parameters.local_contrast,
        parameters.local_contrast_scale,
        parameters.denoise_luminance,
        parameters.denoise_detail,
        parameters.denoise_color,
        parameters.dehaze,
        parameters.defringe_purple_amount,
        parameters.defringe_purple_hue_low,
        parameters.defringe_purple_hue_high,
        parameters.defringe_green_amount,
        parameters.defringe_green_hue_low,
        parameters.defringe_green_hue_high,
        parameters.shadows_hue,
        parameters.shadows_saturation,
        parameters.shadows_luminance,
        parameters.midtones_hue,
        parameters.midtones_saturation,
        parameters.midtones_luminance,
        parameters.highlights_hue,
        parameters.highlights_saturation,
        parameters.highlights_luminance,
        parameters.grading_blending,
        parameters.grading_balance,
        parameters.grain_amount,
        parameters.grain_size,
        parameters.grain_roughness,
        parameters.vignette_amount,
        parameters.vignette_midpoint,
        parameters.vignette_roundness,
        parameters.vignette_feather,
        parameters.vignette_highlights,
    ]
}

pub(crate) fn apply_detail_effect_values(
    parameters: &mut SharpenParameters,
    values: &[f64],
) -> AnyResult<()> {
    let [
        clarity,
        texture,
        local_contrast,
        local_contrast_scale,
        denoise_luminance,
        denoise_detail,
        denoise_color,
        dehaze,
        defringe_purple_amount,
        defringe_purple_hue_low,
        defringe_purple_hue_high,
        defringe_green_amount,
        defringe_green_hue_low,
        defringe_green_hue_high,
        shadows_hue,
        shadows_saturation,
        shadows_luminance,
        midtones_hue,
        midtones_saturation,
        midtones_luminance,
        highlights_hue,
        highlights_saturation,
        highlights_luminance,
        grading_blending,
        grading_balance,
        grain_amount,
        grain_size,
        grain_roughness,
        vignette_amount,
        vignette_midpoint,
        vignette_roundness,
        vignette_feather,
        vignette_highlights,
    ] = values
    else {
        bail!("Detail & Effects storage must contain exactly 33 values");
    };
    parameters.clarity = *clarity;
    parameters.texture = *texture;
    parameters.local_contrast = *local_contrast;
    parameters.local_contrast_scale = *local_contrast_scale;
    parameters.denoise_luminance = *denoise_luminance;
    parameters.denoise_detail = *denoise_detail;
    parameters.denoise_color = *denoise_color;
    parameters.dehaze = *dehaze;
    parameters.defringe_purple_amount = *defringe_purple_amount;
    parameters.defringe_purple_hue_low = *defringe_purple_hue_low;
    parameters.defringe_purple_hue_high = *defringe_purple_hue_high;
    parameters.defringe_green_amount = *defringe_green_amount;
    parameters.defringe_green_hue_low = *defringe_green_hue_low;
    parameters.defringe_green_hue_high = *defringe_green_hue_high;
    parameters.shadows_hue = *shadows_hue;
    parameters.shadows_saturation = *shadows_saturation;
    parameters.shadows_luminance = *shadows_luminance;
    parameters.midtones_hue = *midtones_hue;
    parameters.midtones_saturation = *midtones_saturation;
    parameters.midtones_luminance = *midtones_luminance;
    parameters.highlights_hue = *highlights_hue;
    parameters.highlights_saturation = *highlights_saturation;
    parameters.highlights_luminance = *highlights_luminance;
    parameters.grading_blending = *grading_blending;
    parameters.grading_balance = *grading_balance;
    parameters.grain_amount = *grain_amount;
    parameters.grain_size = *grain_size;
    parameters.grain_roughness = *grain_roughness;
    parameters.vignette_amount = *vignette_amount;
    parameters.vignette_midpoint = *vignette_midpoint;
    parameters.vignette_roundness = *vignette_roundness;
    parameters.vignette_feather = *vignette_feather;
    parameters.vignette_highlights = *vignette_highlights;
    Ok(())
}

pub(crate) fn recipe_detail_effects_render_op(
    id: NodeId,
    operation_id: &str,
    implementation_version: &str,
    stage: ProcessingStage,
    input: NodeInput,
    parameters: &SharpenParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(operation_id)?,
        TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
        implementation_version,
        stage,
        vec![rgb],
        rgb,
        None,
    )?;
    let mut entries = vec![
        (
            SHARPEN_AMOUNT_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.amount)?),
        ),
        (
            SHARPEN_RADIUS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.radius)?),
        ),
        (
            SHARPEN_THRESHOLD_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.threshold)?),
        ),
        (
            SHARPEN_MASKING_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.masking)?),
        ),
    ];
    entries.push((
        DETAIL_EFFECTS_PARAMETERS_KEY,
        ParameterValue::FloatVector(
            detail_effect_values(parameters)
                .into_iter()
                .map(FiniteF64::new)
                .collect::<Result<Vec<_>, _>>()?,
        ),
    ));
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        ParameterBlock::new(values),
        None,
    )
    .map_err(Into::into)
}

pub(crate) fn tone_curve_parameter_value(points: &[ToneCurvePoint]) -> AnyResult<ParameterValue> {
    validate_tone_curve(points)?;
    Ok(ParameterValue::FloatVector(
        points
            .iter()
            .flat_map(|point| [point.x, point.y])
            .map(FiniteF64::new)
            .collect::<Result<Vec<_>, _>>()?,
    ))
}

pub(crate) struct GradeNodeRecipeV1RenderOps<'a> {
    #[cfg(test)]
    pub(crate) layer: &'a LayerInstance,
    pub(crate) exposure: &'a AdjustmentNode,
    pub(crate) contrast: &'a AdjustmentNode,
    pub(crate) selective_tone: &'a AdjustmentNode,
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
    if !(10..=12).contains(&ordered.len()) {
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

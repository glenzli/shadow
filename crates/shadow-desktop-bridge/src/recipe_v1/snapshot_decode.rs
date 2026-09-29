//! Reverse decoding from immutable Recipe v1 snapshots into editable desktop drafts.

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    BasicEditParameters, ColorRangeParameters, OklabLightnessToneCurve,
    PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION, PerceptualColorParameters, SelectiveToneParameters,
    SharpenParameters, ToneCurvePoint,
};
use shadow_domain::operation::{
    BLACKS_PARAMETER_KEY, COLOR_MIXER_HUE_PARAMETER_KEY, COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
    COLOR_MIXER_SATURATION_PARAMETER_KEY, COLOR_RANGE_CENTER_PARAMETER_KEY,
    COLOR_RANGE_ENABLED_PARAMETER_KEY, COLOR_RANGE_HUE_PARAMETER_KEY,
    COLOR_RANGE_LIGHTNESS_PARAMETER_KEY, COLOR_RANGE_SATURATION_PARAMETER_KEY,
    COLOR_RANGE_SOFTNESS_PARAMETER_KEY, COLOR_RANGE_WIDTH_PARAMETER_KEY,
    CONTRAST_FACTOR_PARAMETER_KEY, CONTRAST_PIVOT_PARAMETER_KEY,
    CPU_REFERENCE_IMPLEMENTATION_VERSION, CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
    DETAIL_EFFECTS_PARAMETERS_KEY, EXPOSURE_STOPS_PARAMETER_KEY, GLOBAL_A_BALANCE_PARAMETER_KEY,
    GLOBAL_B_BALANCE_PARAMETER_KEY, HIGHLIGHT_BLUE_SUPPRESSION_PARAMETER_KEY,
    HIGHLIGHT_GREEN_SUPPRESSION_PARAMETER_KEY, HIGHLIGHT_RED_SUPPRESSION_PARAMETER_KEY,
    HIGHLIGHTS_PARAMETER_KEY, LUT_INTENSITY_PARAMETER_KEY, LUT_MANAGED_PATH_PARAMETER_KEY,
    LUT_RESOURCE_ID_PARAMETER_KEY, LUT_TITLE_PARAMETER_KEY,
    OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY, OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
    OKLAB_COLOR_WARPER_OPERATION_ID, OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
    OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID, OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY, PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
    PERCEPTUAL_COLOR_OPERATION_ID, POINT_COLOR_RANGES_PARAMETER_KEY,
    SATURATION_FACTOR_PARAMETER_KEY, SELECTIVE_COLOR_CMYK_PARAMETER_KEY,
    SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY, SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY,
    SELECTIVE_TONE_IMPLEMENTATION_VERSION, SELECTIVE_TONE_OPERATION_ID,
    SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION, SHADOWS_PARAMETER_KEY, SHARPEN_AMOUNT_PARAMETER_KEY,
    SHARPEN_MASKING_PARAMETER_KEY, SHARPEN_RADIUS_PARAMETER_KEY, SHARPEN_THRESHOLD_PARAMETER_KEY,
    TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION, VIBRANCE_PARAMETER_KEY,
    WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY, WHITE_BALANCE_TINT_PARAMETER_KEY,
    WHITES_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EntityId,
    ImageDomain, LayerContent, LayerInstance, LayerInstanceId, LayerRevision,
    LayerRevisionSelector, MaskCoordinateSpace, MaskDefinition, NodeInput, ParameterBlock,
    ParameterKey, ParameterValue, PortType, ProcessingStage, RecipeSnapshot, UnitInterval,
};

use crate::ffi;

use super::snapshot_layout::GradeNodeRecipeV1RenderOps;
use super::{
    CONTRAST_PIVOT, CompositeMaskDraft, FineEditParameters, GradeNodeDraft,
    GradeNodeRecipeV1Identity, GradeStackDraft, LutEditParameters, MAX_GRADE_NODES,
    MaskComponentDraft, MaskComponentDraftDefinition, PreservedManagedRasterSettings,
    SharedGradeNodeReference, apply_detail_effect_values, encode_grade_node_draft_recipe_v1,
    fixed_color_mixer, fixed_selective_color, grade_node_recipe_v1_render_ops,
    is_neutral_oklab_color_warper, oklab_color_warper_from_ffi, point_color_ranges_from_vector,
    recipe_color_grading_render_op_id, recipe_finishing_effects_render_op_id,
    recipe_v1_oklab_color_warper_render_op_id, recipe_v1_oklab_lightness_tone_curve_render_op_id,
    validate_basic_parameters, validate_fine_parameters, validate_grade_stack_draft_recipe_v1,
    validate_tone_curve,
};

#[cfg(test)]
use super::single_grade_node_recipe_v1_render_ops;

#[cfg(test)]
pub(crate) fn basic_parameters_from_snapshot(
    snapshot: &RecipeSnapshot,
) -> AnyResult<BasicEditParameters> {
    if snapshot.layers().is_empty() {
        return Ok(BasicEditParameters::default());
    }
    let nodes = single_grade_node_recipe_v1_render_ops(snapshot)?;
    basic_parameters_from_nodes(&nodes)
}

pub(crate) fn basic_parameters_from_nodes(
    nodes: &GradeNodeRecipeV1RenderOps<'_>,
) -> AnyResult<BasicEditParameters> {
    let exposure_stops =
        required_float(nodes.exposure.parameters(), EXPOSURE_STOPS_PARAMETER_KEY, 1)?;
    let contrast_factor = required_float(
        nodes.contrast.parameters(),
        CONTRAST_FACTOR_PARAMETER_KEY,
        2,
    )?;
    let pivot = required_float(nodes.contrast.parameters(), CONTRAST_PIVOT_PARAMETER_KEY, 2)?;
    if pivot != CONTRAST_PIVOT {
        bail!("working Recipe uses unsupported contrast pivot {pivot}");
    }
    let parameters = BasicEditParameters {
        exposure_stops,
        contrast_factor,
        white_balance_temperature: required_float(
            nodes.white_balance.parameters(),
            WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
            2,
        )?,
        white_balance_tint: required_float(
            nodes.white_balance.parameters(),
            WHITE_BALANCE_TINT_PARAMETER_KEY,
            2,
        )?,
        saturation_factor: required_float(
            nodes.saturation.parameters(),
            SATURATION_FACTOR_PARAMETER_KEY,
            1,
        )?,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
}

// This decoder mirrors the deliberately flat, versioned fine-edit recipe in one place.
#[allow(clippy::too_many_lines)]
pub(crate) fn fine_parameters_from_nodes(
    nodes: &GradeNodeRecipeV1RenderOps<'_>,
) -> AnyResult<FineEditParameters> {
    let selective_tone = selective_tone_parameters(nodes.selective_tone.parameters())?;
    let perceptual_color = {
        let node = nodes.perceptual_color;
        let expected_len = 17;
        PerceptualColorParameters {
            global_a_balance: required_float(
                node.parameters(),
                GLOBAL_A_BALANCE_PARAMETER_KEY,
                expected_len,
            )?,
            global_b_balance: required_float(
                node.parameters(),
                GLOBAL_B_BALANCE_PARAMETER_KEY,
                expected_len,
            )?,
            vibrance: required_float(node.parameters(), VIBRANCE_PARAMETER_KEY, expected_len)?,
            hue_shifts: fixed_color_mixer(
                &required_float_vector(
                    node.parameters(),
                    COLOR_MIXER_HUE_PARAMETER_KEY,
                    expected_len,
                )?,
                "Recipe Color Mixer hue",
            )?,
            saturation: fixed_color_mixer(
                &required_float_vector(
                    node.parameters(),
                    COLOR_MIXER_SATURATION_PARAMETER_KEY,
                    expected_len,
                )?,
                "Recipe Color Mixer saturation",
            )?,
            lightness: fixed_color_mixer(
                &required_float_vector(
                    node.parameters(),
                    COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
                    expected_len,
                )?,
                "Recipe Color Mixer lightness",
            )?,
            color_range: ColorRangeParameters {
                enabled: required_bool(
                    node.parameters(),
                    COLOR_RANGE_ENABLED_PARAMETER_KEY,
                    expected_len,
                )?,
                center_hue_degrees: required_float(
                    node.parameters(),
                    COLOR_RANGE_CENTER_PARAMETER_KEY,
                    expected_len,
                )?,
                width_degrees: required_float(
                    node.parameters(),
                    COLOR_RANGE_WIDTH_PARAMETER_KEY,
                    expected_len,
                )?,
                softness: required_float(
                    node.parameters(),
                    COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
                    expected_len,
                )?,
                hue_shift_degrees: required_float(
                    node.parameters(),
                    COLOR_RANGE_HUE_PARAMETER_KEY,
                    expected_len,
                )?,
                saturation: required_float(
                    node.parameters(),
                    COLOR_RANGE_SATURATION_PARAMETER_KEY,
                    expected_len,
                )?,
                lightness: required_float(
                    node.parameters(),
                    COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
                    expected_len,
                )?,
            },
            additional_color_ranges: point_color_ranges_from_vector(&required_float_vector(
                node.parameters(),
                POINT_COLOR_RANGES_PARAMETER_KEY,
                expected_len,
            )?)?,
            selective_color_relative: required_bool(
                node.parameters(),
                SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY,
                expected_len,
            )?,
            selective_color_lightness_protection: required_float(
                node.parameters(),
                SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
                expected_len,
            )?,
            selective_color_cmyk: fixed_selective_color(&required_float_vector(
                node.parameters(),
                SELECTIVE_COLOR_CMYK_PARAMETER_KEY,
                expected_len,
            )?)?,
        }
    };
    let oklab_color_warper = nodes
        .oklab_color_warper
        .map(|node| {
            oklab_color_warper_from_ffi(
                &required_float_vector(
                    node.parameters(),
                    OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY,
                    2,
                )?,
                required_float(
                    node.parameters(),
                    OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY,
                    2,
                )?,
            )
        })
        .transpose()?
        .unwrap_or_default();
    let oklab_lightness_curve = nodes
        .oklab_lightness_curve
        .map(|node| -> AnyResult<OklabLightnessToneCurve> {
            Ok(OklabLightnessToneCurve {
                lightness: tone_curve_points_from_vector(&required_float_vector(
                    node.parameters(),
                    OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY,
                    1,
                )?)?,
            })
        })
        .transpose()?;
    let lut = {
        let node = nodes.lut;
        let expected_len = 4;
        LutEditParameters {
            resource_id: required_text(
                node.parameters(),
                LUT_RESOURCE_ID_PARAMETER_KEY,
                expected_len,
            )?,
            title: required_text(node.parameters(), LUT_TITLE_PARAMETER_KEY, expected_len)?,
            managed_path: required_text(
                node.parameters(),
                LUT_MANAGED_PATH_PARAMETER_KEY,
                expected_len,
            )?,
            intensity: required_float(
                node.parameters(),
                LUT_INTENSITY_PARAMETER_KEY,
                expected_len,
            )?,
        }
    };
    let sharpen = {
        let node = nodes.technical_detail;
        // The three ordered v1 passes intentionally carry the same visible
        // parameter packet. Reject any hand-edited divergence rather than
        // guessing which copy of a slider should win when a Recipe is read.
        if nodes.color_grading.parameters() != node.parameters()
            || nodes.finishing_effects.parameters() != node.parameters()
        {
            bail!("working Recipe Detail & Effects pass parameters diverge");
        }
        let expected_len = 5;
        let mut parameters = SharpenParameters {
            amount: required_float(
                node.parameters(),
                SHARPEN_AMOUNT_PARAMETER_KEY,
                expected_len,
            )?,
            radius: required_float(
                node.parameters(),
                SHARPEN_RADIUS_PARAMETER_KEY,
                expected_len,
            )?,
            threshold: required_float(
                node.parameters(),
                SHARPEN_THRESHOLD_PARAMETER_KEY,
                expected_len,
            )?,
            masking: required_float(
                node.parameters(),
                SHARPEN_MASKING_PARAMETER_KEY,
                expected_len,
            )?,
            ..SharpenParameters::default()
        };
        apply_detail_effect_values(
            &mut parameters,
            &required_float_vector(
                node.parameters(),
                DETAIL_EFFECTS_PARAMETERS_KEY,
                expected_len,
            )?,
        )?;
        parameters
    };
    let parameters = FineEditParameters {
        selective_tone,
        perceptual_color,
        oklab_color_warper,
        oklab_lightness_curve,
        rgb_tone_curves: nodes
            .rgb_tone_curves
            .map(super::rgb_tone_curves::decode_rgb_tone_curves)
            .transpose()?,
        lut,
        sharpen,
    };
    validate_fine_parameters(&parameters)?;
    Ok(parameters)
}

pub(crate) fn decode_grade_stack_draft_from_recipe_v1_snapshot(
    snapshot: &RecipeSnapshot,
) -> AnyResult<GradeStackDraft> {
    snapshot
        .validate()
        .context("validate persisted Grade Stack Recipe v1")?;
    if snapshot.schema_version() != CURRENT_RECIPE_SCHEMA_VERSION {
        bail!(
            "Grade Stack adapter supports Recipe schema {}, received {}",
            CURRENT_RECIPE_SCHEMA_VERSION,
            snapshot.schema_version()
        );
    }
    if !(1..=MAX_GRADE_NODES).contains(&snapshot.layers().len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    let grade_stack = GradeStackDraft {
        raw_ai_denoise: snapshot.raw_ai_denoise_node(),
        foundation: shadow_domain::PhotoFoundationNode::new(
            snapshot
                .input_settings()
                .clone()
                .with_raw_ai_denoise(shadow_domain::RawFoundationDenoise::disabled()),
        ),
        grade_nodes: snapshot
            .layers()
            .iter()
            .map(|layer| {
                let (local_mask, composite_mask, preserved_managed_raster) =
                    recipe_v1_local_mask_from_snapshot(snapshot, layer)?;
                decode_grade_node_draft_from_recipe_v1_layer(
                    layer,
                    local_mask,
                    composite_mask,
                    preserved_managed_raster,
                )
            })
            .collect::<AnyResult<Vec<_>>>()?,
        retouch_spots: snapshot.retouch_spots().to_vec(),
        retouch_strokes: snapshot.retouch_strokes().to_vec(),
        retouch_enabled: snapshot.retouch_enabled(),
        paint_layers: snapshot.paint_layers().to_vec(),
        image_completions: snapshot.image_completions().to_vec(),
        image_completion_enabled: snapshot.image_completion_enabled(),
        liquify: snapshot.structural_nodes().liquify().cloned(),
        canvas: *snapshot.canvas_node(),
    };
    validate_grade_stack_draft_recipe_v1(&grade_stack)?;
    Ok(grade_stack)
}

pub(crate) fn decode_grade_node_draft_from_recipe_v1_layer(
    layer: &LayerInstance,
    local_mask: Option<MaskDefinition>,
    composite_mask: Option<CompositeMaskDraft>,
    preserved_managed_raster: Option<PreservedManagedRasterSettings>,
) -> AnyResult<GradeNodeDraft> {
    let nodes = grade_node_recipe_v1_render_ops(layer)?;
    if nodes
        .rgb_tone_curves
        .is_some_and(|node| node.id() != super::recipe_v1_rgb_tone_curves_render_op_id(layer.id()))
        || nodes.color_grading.id() != recipe_color_grading_render_op_id(layer.id())
        || nodes.finishing_effects.id() != recipe_finishing_effects_render_op_id(layer.id())
        || nodes.oklab_lightness_curve.is_some_and(|node| {
            node.id() != recipe_v1_oklab_lightness_tone_curve_render_op_id(layer.id())
        })
        || nodes
            .oklab_color_warper
            .is_some_and(|node| node.id() != recipe_v1_oklab_color_warper_render_op_id(layer.id()))
    {
        bail!("working Recipe uses non-canonical internal Detail & Effects pass identities");
    }
    let basic = basic_parameters_from_nodes(&nodes)?;
    let fine = fine_parameters_from_nodes(&nodes)?;
    Ok(GradeNodeDraft {
        recipe_v1_identity: GradeNodeRecipeV1Identity {
            grade_node_id: layer.id(),
            rgb_tone_curves_render_op_id: super::recipe_v1_rgb_tone_curves_render_op_id(layer.id()),
            exposure_render_op_id: nodes.exposure.id(),
            contrast_render_op_id: nodes.contrast.id(),
            oklab_lightness_curve_render_op_id: nodes.oklab_lightness_curve.map_or_else(
                || recipe_v1_oklab_lightness_tone_curve_render_op_id(layer.id()),
                AdjustmentNode::id,
            ),
            selective_tone_render_op_id: nodes.selective_tone.id(),
            white_balance_render_op_id: nodes.white_balance.id(),
            saturation_render_op_id: nodes.saturation.id(),
            perceptual_color_render_op_id: nodes.perceptual_color.id(),
            oklab_color_warper_render_op_id: recipe_v1_oklab_color_warper_render_op_id(layer.id()),
            lut_render_op_id: nodes.lut.id(),
            color_grading_render_op_id: nodes.color_grading.id(),
            sharpen_render_op_id: nodes.technical_detail.id(),
            finishing_effects_render_op_id: nodes.finishing_effects.id(),
        },
        shared: match layer.content() {
            LayerContent::Inline { .. } => None,
            LayerContent::Shared {
                layer_id,
                revision: LayerRevisionSelector::Pinned(revision_id),
                ..
            } => Some(SharedGradeNodeReference {
                layer_id: *layer_id,
                revision_id: *revision_id,
            }),
            LayerContent::Shared {
                revision: LayerRevisionSelector::FollowHead,
                ..
            } => bail!("working shared Grade Node must resolve to a pinned revision"),
        },
        label: layer.label().to_owned(),
        opacity: layer.opacity(),
        local_mask,
        composite_mask,
        preserved_managed_raster,
        basic,
        fine,
        enabled: layer.enabled(),
    })
}

fn validate_editable_condition(node: &shadow_domain::ConditionMaskNode) -> AnyResult<()> {
    use shadow_domain::{ConditionMaskNode, ConditionMaskPredicate};
    match node {
        ConditionMaskNode::Leaf {
            condition: ConditionMaskPredicate::LocalDetailRange { .. },
        } => bail!("local-detail mask conditions are not supported by this desktop renderer"),
        ConditionMaskNode::All { children } | ConditionMaskNode::Any { children } => {
            for child in children {
                validate_editable_condition(child)?;
            }
        }
        ConditionMaskNode::Not { child } => validate_editable_condition(child)?,
        ConditionMaskNode::Leaf { .. } => {}
    }
    Ok(())
}

fn recipe_v1_local_mask_from_snapshot(
    snapshot: &RecipeSnapshot,
    layer: &LayerInstance,
) -> AnyResult<(
    Option<MaskDefinition>,
    Option<CompositeMaskDraft>,
    Option<PreservedManagedRasterSettings>,
)> {
    let Some(reference) = layer.mask() else {
        return Ok((None, None, None));
    };
    if reference.coordinate_space() != MaskCoordinateSpace::Original {
        bail!(
            "Grade Node {} uses unsupported {:?}-space local mask",
            layer.id(),
            reference.coordinate_space()
        );
    }
    let mask = snapshot.resolve_mask(reference).ok_or_else(|| {
        anyhow!(
            "Grade Node {} references local mask {} revision {} that is not stored in this Recipe snapshot",
            layer.id(),
            reference.mask_id(),
            reference.revision()
        )
    })?;
    match mask.definition() {
        MaskDefinition::ConditionExpression { expression } => {
            validate_editable_condition(expression.root())?;
            Ok((Some(mask.definition().clone()), None, None))
        }
        MaskDefinition::ManagedRaster {
            semantic_intent,
            expansion_percent,
            feather_percent,
            invert,
            ..
        } => Ok((
            None,
            None,
            Some(PreservedManagedRasterSettings {
                expansion_percent: *expansion_percent,
                feather_percent: *feather_percent,
                invert: *invert,
                semantic_intent: semantic_intent.clone(),
            }),
        )),
        MaskDefinition::Composite { composite } => {
            let components = composite
                .components()
                .iter()
                .map(|component| {
                    let definition = match component.definition() {
                        MaskDefinition::ConditionExpression { expression } => {
                            validate_editable_condition(expression.root())?;
                            MaskComponentDraftDefinition::Definition(component.definition().clone())
                        }
                        MaskDefinition::ManagedRaster {
                            semantic_intent,
                            expansion_percent,
                            feather_percent,
                            invert,
                            ..
                        } => MaskComponentDraftDefinition::PreservedManagedRaster(
                            PreservedManagedRasterSettings {
                                expansion_percent: *expansion_percent,
                                feather_percent: *feather_percent,
                                invert: *invert,
                                semantic_intent: semantic_intent.clone(),
                            },
                        ),
                        definition => MaskComponentDraftDefinition::Definition(definition.clone()),
                    };
                    Ok(MaskComponentDraft {
                        id: component.id(),
                        operation: component.operation(),
                        enabled: component.enabled(),
                        definition,
                    })
                })
                .collect::<AnyResult<Vec<_>>>()?;
            Ok((
                None,
                Some(CompositeMaskDraft {
                    components,
                    invert: composite.invert(),
                }),
                None,
            ))
        }
        definition => Ok((Some(definition.clone()), None, None)),
    }
}

pub(crate) fn grade_node_draft_from_shared_revision(
    revision: &LayerRevision,
) -> AnyResult<GradeNodeDraft> {
    let layer = LayerInstance::new(
        LayerInstanceId::from_uuid(revision.layer_id().as_uuid()),
        revision.label(),
        AdjustmentScope::Photo,
        LayerContent::Shared {
            layer_id: revision.layer_id(),
            revision: LayerRevisionSelector::Pinned(revision.id()),
            graph: revision.graph().clone(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )?;
    decode_grade_node_draft_from_recipe_v1_layer(&layer, None, None, None)
}

pub(crate) fn ffi_shared_grade_node(
    revision: &LayerRevision,
) -> AnyResult<ffi::FfiSharedGradeNode> {
    Ok(ffi::FfiSharedGradeNode {
        layer_id: revision.layer_id().to_string(),
        revision_id: revision.id().to_string(),
        revision_number: revision.revision_number(),
        label: revision.label().to_owned(),
        grade_node: encode_grade_node_draft_recipe_v1(grade_node_draft_from_shared_revision(
            revision,
        )?)?,
    })
}

pub(crate) fn tone_curve_points_from_vector(flattened: &[f64]) -> AnyResult<Vec<ToneCurvePoint>> {
    if !flattened.len().is_multiple_of(2) {
        bail!("Recipe Tone Curve points must contain flattened x/y pairs");
    }
    let points = flattened
        .chunks_exact(2)
        .map(|point| ToneCurvePoint {
            x: point[0],
            y: point[1],
        })
        .collect::<Vec<_>>();
    validate_tone_curve(&points)?;
    Ok(points)
}

pub(crate) fn validate_recipe_v1_render_op(
    node: &AdjustmentNode,
    operation_id: &str,
    stage: ProcessingStage,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if operation.operation_id().as_str() != operation_id
        || operation.parameter_schema_version() != CPU_REFERENCE_PARAMETER_SCHEMA_VERSION
        || operation.implementation_version() != CPU_REFERENCE_IMPLEMENTATION_VERSION
        || operation.stage() != stage
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe node {operation_id} has an unsupported contract");
    }
    Ok(())
}

pub(crate) fn validate_recipe_oklab_lightness_tone_curve_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Oklab Lightness Curve has an unsupported contract");
    }
    let points = tone_curve_points_from_vector(&required_float_vector(
        node.parameters(),
        OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY,
        1,
    )?)?;
    validate_tone_curve(&points)?;
    Ok(())
}

pub(crate) fn validate_recipe_selective_tone_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == SELECTIVE_TONE_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != SELECTIVE_TONE_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Selective Tone has an unsupported contract");
    }
    Ok(())
}

pub(crate) fn validate_recipe_perceptual_color_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != PERCEPTUAL_COLOR_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Point Color has an unsupported contract");
    }
    Ok(())
}

pub(crate) fn validate_recipe_oklab_color_warper_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != OKLAB_COLOR_WARPER_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Oklab Color Warper has an unsupported contract");
    }
    let parameters = oklab_color_warper_from_ffi(
        &required_float_vector(
            node.parameters(),
            OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY,
            2,
        )?,
        required_float(
            node.parameters(),
            OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY,
            2,
        )?,
    )?;
    if is_neutral_oklab_color_warper(&parameters) {
        bail!("working Recipe must elide a neutral Oklab Color Warper");
    }
    validate_fine_parameters(&FineEditParameters {
        oklab_color_warper: parameters,
        ..FineEditParameters::default()
    })?;
    Ok(())
}

pub(crate) fn validate_recipe_detail_effects_render_op(
    node: &AdjustmentNode,
    operation_id: &str,
    implementation_version: &str,
    stage: ProcessingStage,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == implementation_version;
    if operation.operation_id().as_str() != operation_id
        || !contract_is_supported
        || operation.stage() != stage
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Detail & Effects pass has an unsupported contract");
    }
    Ok(())
}

pub(crate) fn required_float(
    parameters: &ParameterBlock,
    key: &str,
    expected_len: usize,
) -> AnyResult<f64> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::Float(value)) => Ok(value.get()),
        _ => bail!(
            "basic node parameter {} is missing or not a float",
            key.as_str()
        ),
    }
}

pub(crate) fn selective_tone_parameters(
    parameters: &ParameterBlock,
) -> AnyResult<SelectiveToneParameters> {
    const LEGACY_PARAMETER_COUNT: usize = 4;
    const CURRENT_PARAMETER_COUNT: usize = 7;
    let parameter_count = parameters.len();
    if parameter_count != LEGACY_PARAMETER_COUNT && parameter_count != CURRENT_PARAMETER_COUNT {
        bail!("Selective Tone node has unexpected parameter count");
    }

    let legacy = parameter_count == LEGACY_PARAMETER_COUNT;
    Ok(SelectiveToneParameters {
        highlights: required_float(parameters, HIGHLIGHTS_PARAMETER_KEY, parameter_count)?,
        shadows: required_float(parameters, SHADOWS_PARAMETER_KEY, parameter_count)?,
        whites: required_float(parameters, WHITES_PARAMETER_KEY, parameter_count)?,
        blacks: required_float(parameters, BLACKS_PARAMETER_KEY, parameter_count)?,
        highlight_red_suppression: if legacy {
            0.0
        } else {
            required_float(
                parameters,
                HIGHLIGHT_RED_SUPPRESSION_PARAMETER_KEY,
                parameter_count,
            )?
        },
        highlight_green_suppression: if legacy {
            0.0
        } else {
            required_float(
                parameters,
                HIGHLIGHT_GREEN_SUPPRESSION_PARAMETER_KEY,
                parameter_count,
            )?
        },
        highlight_blue_suppression: if legacy {
            0.0
        } else {
            required_float(
                parameters,
                HIGHLIGHT_BLUE_SUPPRESSION_PARAMETER_KEY,
                parameter_count,
            )?
        },
    })
}

pub(crate) fn required_float_vector(
    parameters: &ParameterBlock,
    key: &str,
    expected_len: usize,
) -> AnyResult<Vec<f64>> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::FloatVector(values)) => {
            Ok(values.iter().map(|value| value.get()).collect())
        }
        _ => bail!(
            "basic node parameter {} is missing or not a float vector",
            key.as_str()
        ),
    }
}

pub(crate) fn required_bool(
    parameters: &ParameterBlock,
    key: &str,
    expected_len: usize,
) -> AnyResult<bool> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::Bool(value)) => Ok(*value),
        _ => bail!(
            "basic node parameter {} is missing or not a boolean",
            key.as_str()
        ),
    }
}

pub(crate) fn required_text(
    parameters: &ParameterBlock,
    key: &str,
    expected_len: usize,
) -> AnyResult<String> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::Text(value)) => Ok(value.clone()),
        _ => bail!(
            "basic node parameter {} is missing or not text",
            key.as_str()
        ),
    }
}

#[cfg(test)]
mod tests;

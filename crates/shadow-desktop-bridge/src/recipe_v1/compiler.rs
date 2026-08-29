//! Compilation of persisted Recipe v1 snapshots into executable render plans.

use std::{collections::HashSet, path::Path};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentLiquify,
    AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke, AdjustmentLiquifyReconstructStroke,
    AdjustmentLiquifyStroke, AdjustmentLocalMask, AdjustmentRenderNode, AdjustmentRenderOperation,
    AdjustmentRenderPlan, AdjustmentRetouchStroke, AdjustmentRetouchStrokePoint,
    AdjustmentSpotHealTarget,
    COLOR_GRADING_IMPLEMENTATION_VERSION as COLOR_GRADING_IMPLEMENTATION_REVISION,
    ColorRangeParameters,
    FINISHING_EFFECTS_IMPLEMENTATION_VERSION as FINISHING_EFFECTS_IMPLEMENTATION_REVISION,
    MAX_ADJUSTMENT_RENDER_NODES, MAX_LUT_DOCUMENT_BYTES,
    OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION as OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_REVISION,
    OklabLightnessToneCurve,
    PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION as PERCEPTUAL_COLOR_IMPLEMENTATION_REVISION,
    PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION, PerceptualColorParameters,
    SELECTIVE_TONE_IMPLEMENTATION_VERSION as SELECTIVE_TONE_IMPLEMENTATION_REVISION,
    SharpenParameters,
    TECHNICAL_DETAIL_IMPLEMENTATION_VERSION as TECHNICAL_DETAIL_IMPLEMENTATION_REVISION,
};
use shadow_domain::operation::{
    COLOR_GRADING_IMPLEMENTATION_VERSION, COLOR_GRADING_OPERATION_ID,
    COLOR_GRADING_PARAMETER_SCHEMA_VERSION, COLOR_MIXER_HUE_PARAMETER_KEY,
    COLOR_MIXER_LIGHTNESS_PARAMETER_KEY, COLOR_MIXER_SATURATION_PARAMETER_KEY,
    COLOR_RANGE_CENTER_PARAMETER_KEY, COLOR_RANGE_ENABLED_PARAMETER_KEY,
    COLOR_RANGE_HUE_PARAMETER_KEY, COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
    COLOR_RANGE_SATURATION_PARAMETER_KEY, COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
    COLOR_RANGE_WIDTH_PARAMETER_KEY, CONTRAST_FACTOR_PARAMETER_KEY, CONTRAST_OPERATION_ID,
    CONTRAST_PIVOT_PARAMETER_KEY, CPU_REFERENCE_IMPLEMENTATION_REVISION,
    CPU_REFERENCE_IMPLEMENTATION_VERSION, CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
    DETAIL_EFFECTS_PARAMETERS_KEY, EXPOSURE_OPERATION_ID, EXPOSURE_STOPS_PARAMETER_KEY,
    FINISHING_EFFECTS_IMPLEMENTATION_VERSION, FINISHING_EFFECTS_OPERATION_ID,
    FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION, GLOBAL_A_BALANCE_PARAMETER_KEY,
    GLOBAL_B_BALANCE_PARAMETER_KEY, LUT_3D_OPERATION_ID, LUT_INTENSITY_PARAMETER_KEY,
    LUT_MANAGED_PATH_PARAMETER_KEY, LUT_RESOURCE_ID_PARAMETER_KEY, LUT_TITLE_PARAMETER_KEY,
    OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY, OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
    OKLAB_COLOR_WARPER_OPERATION_ID, OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
    OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID, OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY, PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
    PERCEPTUAL_COLOR_OPERATION_ID, POINT_COLOR_RANGES_PARAMETER_KEY,
    RGB_WHITE_BALANCE_OPERATION_ID, SATURATION_FACTOR_PARAMETER_KEY, SATURATION_OPERATION_ID,
    SELECTIVE_COLOR_CMYK_PARAMETER_KEY, SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
    SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY, SELECTIVE_TONE_IMPLEMENTATION_VERSION,
    SELECTIVE_TONE_OPERATION_ID, SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
    SHARPEN_AMOUNT_PARAMETER_KEY, SHARPEN_MASKING_PARAMETER_KEY, SHARPEN_RADIUS_PARAMETER_KEY,
    SHARPEN_THRESHOLD_PARAMETER_KEY, TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
    TECHNICAL_DETAIL_OPERATION_ID, TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
    VIBRANCE_PARAMETER_KEY, WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
    WHITE_BALANCE_TINT_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, CURRENT_RECIPE_SCHEMA_VERSION, ImageDomain, LayerInstanceId, LiquifyStroke,
    MaskCoordinateSpace, MaskDefinition, PhotoLiquifyNode, PortType, ProcessingStage,
    RecipeSnapshot, RetouchMode,
};

use super::{
    MAX_GRADE_NODES, apply_detail_effect_values, ffi_adapter::adjustment_geometry,
    fixed_color_mixer, fixed_selective_color, grade_node_recipe_v1_render_ops,
    managed_raster_resolution::ManagedRasterMaskResolver, oklab_color_warper_from_ffi,
    point_color_ranges_from_vector, required_bool, required_float, required_float_vector,
    required_text, selective_tone_parameters, tone_curve_points_from_vector,
};

// Retouch is photo-local rather than a Grade Node. These fixed, compiler-only
// stream identities keep it deterministic without making it user-visible or
// shareable by mistake.
const RECIPE_V1_RETOUCH_LAYER_START_ID: &str = "recipe-v1-photo-retouch:start";
const RECIPE_V1_RETOUCH_RENDER_NODE_ID: &str = "recipe-v1-photo-retouch:spots";
const RECIPE_V1_RETOUCH_LAYER_END_ID: &str = "recipe-v1-photo-retouch:end";

/// Compiles the currently executable Recipe v1 adapter subset into dependency
/// order. Recipe `LayerInstance` vector order is the Grade Node execution
/// order; graph bindings and the explicit output node define the private
/// render-operation order within each Grade Node.
#[cfg(test)]
pub(crate) fn compile_recipe_render_plan(
    snapshot: &RecipeSnapshot,
) -> AnyResult<AdjustmentRenderPlan> {
    compile_recipe_render_plan_with_resolver(snapshot, None)
}

pub(crate) fn compile_recipe_render_plan_with_managed_rasters(
    snapshot: &RecipeSnapshot,
    resolver: &dyn ManagedRasterMaskResolver,
) -> AnyResult<AdjustmentRenderPlan> {
    compile_recipe_render_plan_with_resolver(snapshot, Some(resolver))
}

fn compile_recipe_render_plan_with_resolver(
    snapshot: &RecipeSnapshot,
    resolver: Option<&dyn ManagedRasterMaskResolver>,
) -> AnyResult<AdjustmentRenderPlan> {
    validate_recipe_compilation_contract(snapshot)?;
    // A photo-local repair must run after every Grade Node. It uses an
    // unmasked boundary layer so the native executor can keep one ordering
    // grammar for both local Grade Nodes and photo-local spatial operations.
    let has_retouch =
        !snapshot.retouch_spots().is_empty() || !snapshot.retouch_strokes().is_empty();
    let use_layer_boundaries = has_retouch
        || snapshot.layers().iter().any(|layer| {
            layer.mask().is_some() || layer.opacity() != shadow_domain::UnitInterval::ONE
        });
    let mut compiled = Vec::new();
    let mut compiled_node_ids = HashSet::new();
    for layer in snapshot.layers() {
        if use_layer_boundaries {
            let mask = match layer.mask() {
                None => None,
                Some(reference) => {
                    if reference.coordinate_space() != MaskCoordinateSpace::Original {
                        bail!(
                            "Recipe layer {} uses unsupported {:?}-space local mask",
                            layer.id(),
                            reference.coordinate_space()
                        );
                    }
                    let definition = snapshot.resolve_mask(reference).ok_or_else(|| {
                        anyhow!(
                            "Recipe layer {} references local mask {} revision {} that is not stored in this Recipe snapshot",
                            layer.id(),
                            reference.mask_id(),
                            reference.revision()
                        )
                    })?;
                    Some(adjustment_local_mask_with_resolver(
                        definition.definition(),
                        resolver,
                    )?)
                }
            };
            let start_id = format!("local-mask-layer-start:{}", layer.id());
            let end_id = format!("local-mask-layer-end:{}", layer.id());
            for boundary_id in [&start_id, &end_id] {
                if !compiled_node_ids.insert(boundary_id.clone()) {
                    bail!(
                        "Recipe render compiler rejects duplicate local-mask boundary id {boundary_id}"
                    );
                }
            }
            compiled.push(AdjustmentRenderNode {
                node_id: start_id,
                parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                enabled: layer.enabled(),
                operation: AdjustmentRenderOperation::LocalMaskLayerStart {
                    opacity: layer.opacity().get(),
                    mask,
                },
            });
        }
        let nodes = grade_node_recipe_v1_render_ops(layer)?;
        for node in nodes.ordered() {
            if !compiled_node_ids.insert(node.id().to_string()) {
                bail!(
                    "Recipe v1 render compiler rejects duplicate render-op id {}",
                    node.id()
                );
            }
            compiled.push(compile_recipe_node(
                node,
                layer.id(),
                if use_layer_boundaries {
                    true
                } else {
                    layer.enabled()
                },
            )?);
        }
        if use_layer_boundaries {
            compiled.push(AdjustmentRenderNode {
                node_id: format!("local-mask-layer-end:{}", layer.id()),
                parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                enabled: true,
                operation: AdjustmentRenderOperation::LocalMaskLayerEnd,
            });
        }
    }
    if has_retouch {
        append_photo_retouch_nodes(snapshot, &mut compiled, &mut compiled_node_ids)?;
    }
    if compiled.len() > MAX_ADJUSTMENT_RENDER_NODES {
        bail!("Recipe render compiler supports at most 256 executable nodes");
    }
    let plan = AdjustmentRenderPlan {
        nodes: compiled,
        liquify: snapshot
            .structural_nodes()
            .liquify()
            .map(adjustment_liquify),
        geometry: adjustment_geometry(snapshot.geometry()),
    };
    plan.validate()
        .context("validate compiled Recipe render plan")?;
    Ok(plan)
}

fn validate_recipe_compilation_contract(snapshot: &RecipeSnapshot) -> AnyResult<()> {
    snapshot
        .validate()
        .context("validate Recipe before rendering")?;
    if snapshot.schema_version() != CURRENT_RECIPE_SCHEMA_VERSION {
        bail!(
            "Recipe render compiler supports schema {}, received {}",
            CURRENT_RECIPE_SCHEMA_VERSION,
            snapshot.schema_version()
        );
    }
    if !(1..=MAX_GRADE_NODES).contains(&snapshot.layers().len()) {
        bail!("Recipe v1 render compiler supports 1 through 16 Grade Nodes");
    }
    Ok(())
}

fn adjustment_liquify(liquify: &PhotoLiquifyNode) -> AdjustmentLiquify {
    AdjustmentLiquify {
        enabled: liquify.enabled(),
        strokes: liquify
            .strokes()
            .iter()
            .map(|stroke| match stroke {
                LiquifyStroke::Push {
                    points,
                    radius,
                    strength,
                    hardness,
                } => AdjustmentLiquifyStroke::Push(AdjustmentLiquifyPushStroke {
                    points: points
                        .iter()
                        .map(|point| AdjustmentLiquifyPoint {
                            x: point.x().get(),
                            y: point.y().get(),
                            pressure: point.pressure().get(),
                        })
                        .collect(),
                    radius: radius.get(),
                    strength: strength.get(),
                    hardness: hardness.get(),
                }),
                LiquifyStroke::Reconstruct {
                    points,
                    radius,
                    strength,
                    hardness,
                } => AdjustmentLiquifyStroke::Reconstruct(AdjustmentLiquifyReconstructStroke {
                    points: points
                        .iter()
                        .map(|point| AdjustmentLiquifyPoint {
                            x: point.x().get(),
                            y: point.y().get(),
                            pressure: point.pressure().get(),
                        })
                        .collect(),
                    radius: radius.get(),
                    strength: strength.get(),
                    hardness: hardness.get(),
                }),
            })
            .collect(),
    }
}

/// Appends the photo-local repair stage after every Grade Node. Retouch owns a
/// separate unmasked layer so it cannot inherit the final local adjustment's
/// mask or enabled state.
fn append_photo_retouch_nodes(
    snapshot: &RecipeSnapshot,
    compiled: &mut Vec<AdjustmentRenderNode>,
    compiled_node_ids: &mut HashSet<String>,
) -> AnyResult<()> {
    for boundary_id in [
        RECIPE_V1_RETOUCH_LAYER_START_ID,
        RECIPE_V1_RETOUCH_RENDER_NODE_ID,
        RECIPE_V1_RETOUCH_LAYER_END_ID,
    ] {
        if !compiled_node_ids.insert(boundary_id.to_owned()) {
            bail!("Recipe render compiler rejects duplicate photo-retouch id {boundary_id}");
        }
    }
    compiled.push(AdjustmentRenderNode {
        node_id: RECIPE_V1_RETOUCH_LAYER_START_ID.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerStart {
            opacity: 1.0,
            mask: None,
        },
    });
    compiled.push(AdjustmentRenderNode {
        node_id: RECIPE_V1_RETOUCH_RENDER_NODE_ID.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: snapshot.retouch_enabled(),
        operation: AdjustmentRenderOperation::SpotHeal {
            targets: snapshot
                .retouch_spots()
                .iter()
                .map(|spot| AdjustmentSpotHealTarget {
                    center_x: spot.center_x().get(),
                    center_y: spot.center_y().get(),
                    radius_level_zero_pixels: spot.radius_level_zero_pixels(),
                    mode: match spot.mode() {
                        RetouchMode::Heal => 0,
                        RetouchMode::Clone => 1,
                        RetouchMode::HealStructure => 2,
                    },
                    source_offset_x_radii: spot.source_offset_x_radii(),
                    source_offset_y_radii: spot.source_offset_y_radii(),
                    source_rotation_degrees: spot.source_rotation_degrees(),
                    source_scale: spot.source_scale(),
                    source_flip_horizontal: spot.source_flip_horizontal(),
                    source_flip_vertical: spot.source_flip_vertical(),
                    feather: spot.feather().get(),
                    strength: spot.strength().get(),
                })
                .collect(),
            strokes: snapshot
                .retouch_strokes()
                .iter()
                .map(|stroke| AdjustmentRetouchStroke {
                    points: stroke
                        .points()
                        .iter()
                        .map(|point| AdjustmentRetouchStrokePoint {
                            x: point.x().get(),
                            y: point.y().get(),
                        })
                        .collect(),
                    radius_level_zero_pixels: stroke.radius_level_zero_pixels(),
                    mode: match stroke.mode() {
                        RetouchMode::Heal => 0,
                        RetouchMode::Clone => 1,
                        RetouchMode::HealStructure => 2,
                    },
                    source_offset_x_radii: stroke.source_offset_x_radii(),
                    source_offset_y_radii: stroke.source_offset_y_radii(),
                    source_rotation_degrees: stroke.source_rotation_degrees(),
                    source_scale: stroke.source_scale(),
                    source_flip_horizontal: stroke.source_flip_horizontal(),
                    source_flip_vertical: stroke.source_flip_vertical(),
                    feather: stroke.feather().get(),
                    strength: stroke.strength().get(),
                })
                .collect(),
        },
    });
    compiled.push(AdjustmentRenderNode {
        node_id: RECIPE_V1_RETOUCH_LAYER_END_ID.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerEnd,
    });
    Ok(())
}

#[cfg(test)]
fn adjustment_local_mask(definition: &MaskDefinition) -> AnyResult<AdjustmentLocalMask> {
    adjustment_local_mask_with_resolver(definition, None)
}

fn adjustment_local_mask_with_resolver(
    definition: &MaskDefinition,
    resolver: Option<&dyn ManagedRasterMaskResolver>,
) -> AnyResult<AdjustmentLocalMask> {
    Ok(match definition {
        MaskDefinition::LinearGradient {
            start_x,
            start_y,
            end_x,
            end_y,
            invert,
        } => AdjustmentLocalMask::LinearGradient {
            start_x: start_x.get(),
            start_y: start_y.get(),
            end_x: end_x.get(),
            end_y: end_y.get(),
            invert: *invert,
        },
        MaskDefinition::RadialGradient {
            center_x,
            center_y,
            radius_x,
            radius_y,
            feather,
            invert,
        } => AdjustmentLocalMask::RadialGradient {
            center_x: center_x.get(),
            center_y: center_y.get(),
            radius_x: radius_x.get(),
            radius_y: radius_y.get(),
            feather: feather.get(),
            invert: *invert,
        },
        MaskDefinition::Brush {
            points,
            radius,
            feather,
            invert,
        } => AdjustmentLocalMask::Brush {
            points: points
                .iter()
                .map(|point| shadow_bridge::AdjustmentMaskBrushPoint {
                    x: point.x().get(),
                    y: point.y().get(),
                    begins_stroke: point.begins_stroke(),
                })
                .collect(),
            radius: radius.get(),
            feather: feather.get(),
            invert: *invert,
        },
        MaskDefinition::LuminanceRange {
            lower,
            upper,
            softness,
            invert,
        } => AdjustmentLocalMask::LuminanceRange {
            lower: lower.get(),
            upper: upper.get(),
            softness: softness.get(),
            invert: *invert,
        },
        MaskDefinition::ColorRange {
            center_hue_degrees,
            width_degrees,
            softness,
            invert,
        } => AdjustmentLocalMask::ColorRange {
            center_hue_degrees: center_hue_degrees.get(),
            width_degrees: width_degrees.get(),
            softness: softness.get(),
            invert: *invert,
        },
        MaskDefinition::ConditionExpression { .. } => {
            bail!(
                "Recipe v1 persists bounded condition-mask expressions, but this renderer supports only single luminance and zero-minimum-chroma hue leaves"
            )
        }
        MaskDefinition::ManagedRaster {
            raster,
            expansion_percent,
            feather_percent,
            invert,
        } => resolver
            .ok_or_else(|| {
                anyhow!(
                    "Recipe v1 managed raster mask requires verified application-store resolution"
                )
            })?
            .resolve(
                raster,
                f64::from(*expansion_percent) / 100.0,
                f64::from(*feather_percent) / 100.0,
                *invert,
            )?,
    })
}

#[allow(clippy::too_many_lines)] // Keep the exhaustive operation-contract mapping auditable.
pub(crate) fn compile_recipe_node(
    node: &AdjustmentNode,
    layer_id: LayerInstanceId,
    grade_node_enabled: bool,
) -> AnyResult<AdjustmentRenderNode> {
    let descriptor = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let is_base_contract = descriptor.parameter_schema_version()
        == CPU_REFERENCE_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == CPU_REFERENCE_IMPLEMENTATION_VERSION;
    let is_current_oklab_lightness_tone_curve = descriptor.operation_id().as_str()
        == OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID
        && descriptor.parameter_schema_version()
            == OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION;
    let is_current_selective_tone = descriptor.operation_id().as_str()
        == SELECTIVE_TONE_OPERATION_ID
        && descriptor.parameter_schema_version() == SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == SELECTIVE_TONE_IMPLEMENTATION_VERSION;
    let is_current_perceptual_color = descriptor.operation_id().as_str()
        == PERCEPTUAL_COLOR_OPERATION_ID
        && descriptor.parameter_schema_version() == PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION;
    let is_current_oklab_color_warper = descriptor.operation_id().as_str()
        == OKLAB_COLOR_WARPER_OPERATION_ID
        && descriptor.parameter_schema_version() == OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION;
    let is_current_technical_detail = descriptor.operation_id().as_str()
        == TECHNICAL_DETAIL_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == TECHNICAL_DETAIL_IMPLEMENTATION_VERSION;
    let is_current_color_grading = descriptor.operation_id().as_str() == COLOR_GRADING_OPERATION_ID
        && descriptor.parameter_schema_version() == COLOR_GRADING_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == COLOR_GRADING_IMPLEMENTATION_VERSION;
    let is_current_finishing_effects = descriptor.operation_id().as_str()
        == FINISHING_EFFECTS_OPERATION_ID
        && descriptor.parameter_schema_version() == FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == FINISHING_EFFECTS_IMPLEMENTATION_VERSION;
    if (!is_base_contract
        && !is_current_oklab_lightness_tone_curve
        && !is_current_selective_tone
        && !is_current_perceptual_color
        && !is_current_oklab_color_warper
        && !is_current_technical_detail
        && !is_current_color_grading
        && !is_current_finishing_effects)
        || descriptor.input_types() != [rgb]
        || descriptor.output_type() != rgb
        || descriptor.seed().is_some()
        || node.mask_reference().is_some()
    {
        bail!(
            "Recipe node {} uses an unsupported operation contract, seed, or mask",
            node.id()
        );
    }

    let operation = match descriptor.operation_id().as_str() {
        EXPOSURE_OPERATION_ID => {
            require_stage(node, ProcessingStage::SceneLinearFoundation)?;
            AdjustmentRenderOperation::Exposure {
                stops: required_float(node.parameters(), EXPOSURE_STOPS_PARAMETER_KEY, 1)?,
            }
        }
        CONTRAST_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            AdjustmentRenderOperation::Contrast {
                factor: required_float(node.parameters(), CONTRAST_FACTOR_PARAMETER_KEY, 2)?,
                pivot: required_float(node.parameters(), CONTRAST_PIVOT_PARAMETER_KEY, 2)?,
            }
        }
        OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_oklab_lightness_tone_curve {
                bail!("Recipe Oklab Lightness Curve uses a discarded contract");
            }
            AdjustmentRenderOperation::OklabLightnessToneCurve {
                curve: Box::new(OklabLightnessToneCurve {
                    lightness: tone_curve_points_from_vector(&required_float_vector(
                        node.parameters(),
                        OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY,
                        1,
                    )?)?,
                }),
            }
        }
        RGB_WHITE_BALANCE_OPERATION_ID => {
            require_stage(node, ProcessingStage::SceneLinearFoundation)?;
            AdjustmentRenderOperation::RgbWhiteBalance {
                temperature: required_float(
                    node.parameters(),
                    WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
                    2,
                )?,
                tint: required_float(node.parameters(), WHITE_BALANCE_TINT_PARAMETER_KEY, 2)?,
            }
        }
        SATURATION_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            AdjustmentRenderOperation::Saturation {
                factor: required_float(node.parameters(), SATURATION_FACTOR_PARAMETER_KEY, 1)?,
            }
        }
        SELECTIVE_TONE_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_selective_tone {
                bail!("Recipe Selective Tone uses a discarded contract");
            }
            AdjustmentRenderOperation::SelectiveTone {
                parameters: selective_tone_parameters(node.parameters())?,
            }
        }
        PERCEPTUAL_COLOR_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_perceptual_color {
                bail!("Recipe Color Mixer uses a discarded contract");
            }
            let expected_len = 17;
            AdjustmentRenderOperation::PerceptualColor {
                parameters: Box::new(PerceptualColorParameters {
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
                    vibrance: required_float(
                        node.parameters(),
                        VIBRANCE_PARAMETER_KEY,
                        expected_len,
                    )?,
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
                    additional_color_ranges: point_color_ranges_from_vector(
                        &required_float_vector(
                            node.parameters(),
                            POINT_COLOR_RANGES_PARAMETER_KEY,
                            expected_len,
                        )?,
                    )?,
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
                }),
            }
        }
        OKLAB_COLOR_WARPER_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_oklab_color_warper {
                bail!("Recipe Oklab Color Warper uses a discarded contract");
            }
            let expected_len = 2;
            AdjustmentRenderOperation::OklabColorWarper {
                parameters: Box::new(oklab_color_warper_from_ffi(
                    &required_float_vector(
                        node.parameters(),
                        OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY,
                        expected_len,
                    )?,
                    required_float(
                        node.parameters(),
                        OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY,
                        expected_len,
                    )?,
                )?),
            }
        }
        LUT_3D_OPERATION_ID => {
            require_stage(node, ProcessingStage::CreativeColor)?;
            let expected_len = 4;
            let resource_id = required_text(
                node.parameters(),
                LUT_RESOURCE_ID_PARAMETER_KEY,
                expected_len,
            )?;
            let _title = required_text(node.parameters(), LUT_TITLE_PARAMETER_KEY, expected_len)?;
            let managed_path = required_text(
                node.parameters(),
                LUT_MANAGED_PATH_PARAMETER_KEY,
                expected_len,
            )?;
            let requested_intensity =
                required_float(node.parameters(), LUT_INTENSITY_PARAMETER_KEY, expected_len)?;
            if resource_id.is_empty() {
                if !managed_path.is_empty() {
                    bail!("unselected LUT has a managed path");
                }
                AdjustmentRenderOperation::Lut3D {
                    document: Vec::new(),
                    intensity: 0.0,
                }
            } else {
                let path = Path::new(&managed_path);
                if resource_id.len() != 64
                    || !resource_id
                        .bytes()
                        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
                    || !path.is_absolute()
                    || path.extension().and_then(|value| value.to_str()) != Some("cube")
                    || path.file_stem().and_then(|value| value.to_str())
                        != Some(resource_id.as_str())
                {
                    bail!("Recipe LUT does not reference a content-addressed managed resource");
                }
                let metadata = std::fs::metadata(path)
                    .with_context(|| format!("inspect managed LUT {managed_path:?}"))?;
                if metadata.len() == 0
                    || metadata.len() > u64::try_from(MAX_LUT_DOCUMENT_BYTES).unwrap()
                {
                    bail!("managed LUT must contain 1 byte through 16 MiB");
                }
                AdjustmentRenderOperation::Lut3D {
                    document: std::fs::read(path)
                        .with_context(|| format!("read managed LUT {managed_path:?}"))?,
                    intensity: requested_intensity,
                }
            }
        }
        TECHNICAL_DETAIL_OPERATION_ID
        | COLOR_GRADING_OPERATION_ID
        | FINISHING_EFFECTS_OPERATION_ID => {
            let expected_stage = if is_current_technical_detail {
                ProcessingStage::TechnicalDetail
            } else if is_current_color_grading {
                ProcessingStage::CreativeColor
            } else if is_current_finishing_effects {
                ProcessingStage::FinishingEffects
            } else {
                bail!("Recipe Detail & Effects uses a discarded contract");
            };
            require_stage(node, expected_stage)?;
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
            let pass = if is_current_technical_detail {
                shadow_bridge::AdjustmentDetailEffectsPass::TechnicalDetail
            } else if is_current_color_grading {
                shadow_bridge::AdjustmentDetailEffectsPass::ColorGrading
            } else {
                shadow_bridge::AdjustmentDetailEffectsPass::FinishingEffects
            };
            AdjustmentRenderOperation::Sharpen {
                pass,
                parameters: Box::new(parameters),
            }
        }
        operation_id => bail!("Recipe operation {operation_id:?} is not executable by this build"),
    };
    Ok(AdjustmentRenderNode {
        // NodeId uniqueness is a graph invariant, not a snapshot-wide domain
        // invariant. Namespacing preserves exact diagnostic identity after the
        // layer graphs are flattened into one executor plan.
        node_id: format!("{layer_id}/{}", node.id()),
        parameter_schema_version: descriptor.parameter_schema_version(),
        implementation_version: if is_current_selective_tone {
            SELECTIVE_TONE_IMPLEMENTATION_REVISION
        } else if is_current_perceptual_color {
            PERCEPTUAL_COLOR_IMPLEMENTATION_REVISION
        } else if is_current_oklab_lightness_tone_curve {
            OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_REVISION
        } else if is_current_technical_detail {
            TECHNICAL_DETAIL_IMPLEMENTATION_REVISION
        } else if is_current_color_grading {
            COLOR_GRADING_IMPLEMENTATION_REVISION
        } else if is_current_finishing_effects {
            FINISHING_EFFECTS_IMPLEMENTATION_REVISION
        } else {
            CPU_REFERENCE_IMPLEMENTATION_REVISION
        },
        enabled: grade_node_enabled,
        operation,
    })
}

pub(crate) fn require_stage(node: &AdjustmentNode, expected: ProcessingStage) -> AnyResult<()> {
    if node.operation().stage() == expected {
        Ok(())
    } else {
        bail!(
            "Recipe node {} has stage {:?}; expected {:?}",
            node.id(),
            node.operation().stage(),
            expected
        )
    }
}

#[cfg(test)]
mod tests;

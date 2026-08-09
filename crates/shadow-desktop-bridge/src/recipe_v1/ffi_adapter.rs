//! Bidirectional translation between Qt FFI edit DTOs and Recipe v1 drafts.
//!
//! This boundary also owns spatial-mask, photo-geometry, and optics DTO
//! conversion because those values enter and leave through the same atomic
//! editable Grade Stack contract.

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    AdjustmentGeometry, AdjustmentQuarterTurn, BasicEditParameters, COLOR_MIXER_BAND_COUNT,
    ColorRangeParameters, OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT, OklabColorWarperControlPoint,
    OklabColorWarperParameters, OklabLightnessToneCurve, PerceptualColorParameters,
    SELECTIVE_COLOR_VALUE_COUNT, SelectiveToneParameters, SharpenParameters,
};
use shadow_domain::{
    LayerId, LayerInstanceId, LayerRevisionId, MAX_MASK_BRUSH_POINTS, MaskBrushPoint,
    MaskDefinition, NodeId, PhotoCanvasNode, PhotoFoundationNode, PhotoGeometry, PhotoQuarterTurn,
    RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN, RawFoundationDenoise, RawFoundationDenoiseModel,
    RawTemperatureTint, RawWhiteBalance, RecipeInputSettings, RecipeOpticsSettings, RetouchMode,
    RetouchPoint, RetouchSpot, RetouchStroke, UnitInterval,
};

use crate::ffi;

use super::{
    FineEditParameters, GradeNodeDraft, GradeNodeRecipeV1Identity, GradeStackDraft,
    LutEditParameters, MAX_GRADE_NODES, PreservedManagedRasterSettings, SharedGradeNodeReference,
    grade_stack_recipe_v1_snapshot, point_color_ranges_from_vector_optional,
    recipe_color_grading_render_op_id, recipe_finishing_effects_render_op_id,
    recipe_v1_oklab_color_warper_render_op_id, recipe_v1_oklab_lightness_tone_curve_render_op_id,
    tone_curve_points_from_vector, validate_basic_parameters, validate_fine_parameters,
    validate_grade_stack_draft_recipe_v1,
};

mod liquify;

use liquify::photo_liquify_from_ffi;

const LOCAL_MASK_NONE: u8 = 0;
const LOCAL_MASK_LINEAR_GRADIENT: u8 = 1;
const LOCAL_MASK_RADIAL_GRADIENT: u8 = 2;
const LOCAL_MASK_BRUSH: u8 = 3;
const LOCAL_MASK_LUMINANCE_RANGE: u8 = 4;
const LOCAL_MASK_COLOR_RANGE: u8 = 5;
const LOCAL_MASK_MANAGED_RASTER: u8 = 6;
const RAW_WHITE_BALANCE_AS_SHOT: u8 = 0;
const RAW_WHITE_BALANCE_TEMPERATURE_TINT: u8 = 1;
const RAW_AI_DENOISE_MODEL_RAWNIND_PUBLIC_BAYER_RELEASE_5_6_0: u8 = 0;

type FfiLocalMaskFields = (u8, f64, f64, f64, f64, f64, f64, f64, bool, Vec<f64>);

// Keep the complete mask sum-type projection together: every alternative
// participates in one atomic CXX Grade Node record.
#[allow(clippy::too_many_lines)]
fn ffi_local_mask_fields(
    mask: Option<&MaskDefinition>,
    preserved_managed_raster: Option<PreservedManagedRasterSettings>,
) -> AnyResult<FfiLocalMaskFields> {
    if mask.is_some() && preserved_managed_raster.is_some() {
        bail!("Grade Node cannot project two local-mask representations at once");
    }
    if let Some(settings) = preserved_managed_raster {
        return Ok((
            LOCAL_MASK_MANAGED_RASTER,
            f64::from(settings.expansion_percent) / 100.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            f64::from(settings.feather_percent) / 100.0,
            settings.invert,
            Vec::new(),
        ));
    }
    Ok(match mask {
        None => (
            LOCAL_MASK_NONE,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            false,
            Vec::new(),
        ),
        Some(MaskDefinition::LinearGradient {
            start_x,
            start_y,
            end_x,
            end_y,
            invert,
        }) => (
            LOCAL_MASK_LINEAR_GRADIENT,
            start_x.get(),
            start_y.get(),
            end_x.get(),
            end_y.get(),
            0.0,
            0.0,
            0.0,
            *invert,
            Vec::new(),
        ),
        Some(MaskDefinition::RadialGradient {
            center_x,
            center_y,
            radius_x,
            radius_y,
            feather,
            invert,
        }) => (
            LOCAL_MASK_RADIAL_GRADIENT,
            center_x.get(),
            center_y.get(),
            0.0,
            0.0,
            radius_x.get(),
            radius_y.get(),
            feather.get(),
            *invert,
            Vec::new(),
        ),
        Some(MaskDefinition::Brush {
            points,
            radius,
            feather,
            invert,
        }) => (
            LOCAL_MASK_BRUSH,
            0.0,
            0.0,
            0.0,
            0.0,
            radius.get(),
            0.0,
            feather.get(),
            *invert,
            points
                .iter()
                .flat_map(|point| {
                    [
                        point.x().get(),
                        point.y().get(),
                        if point.begins_stroke() { 1.0 } else { 0.0 },
                    ]
                })
                .collect(),
        ),
        Some(MaskDefinition::LuminanceRange {
            lower,
            upper,
            softness,
            invert,
        }) => (
            LOCAL_MASK_LUMINANCE_RANGE,
            lower.get(),
            0.0,
            upper.get(),
            0.0,
            0.0,
            0.0,
            softness.get(),
            *invert,
            Vec::new(),
        ),
        Some(MaskDefinition::ColorRange {
            center_hue_degrees,
            width_degrees,
            softness,
            invert,
        }) => (
            LOCAL_MASK_COLOR_RANGE,
            center_hue_degrees.get() / 360.0,
            0.0,
            width_degrees.get() / 180.0,
            0.0,
            0.0,
            0.0,
            softness.get(),
            *invert,
            Vec::new(),
        ),
        Some(MaskDefinition::ConditionExpression { .. }) => {
            bail!(
                "the current Qt Grade Node DTO cannot represent composite, chroma-qualified, or local-detail condition masks"
            )
        }
        Some(MaskDefinition::ManagedRaster {
            expansion_percent,
            feather_percent,
            invert,
            ..
        }) => (
            LOCAL_MASK_MANAGED_RASTER,
            f64::from(*expansion_percent) / 100.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            f64::from(*feather_percent) / 100.0,
            *invert,
            Vec::new(),
        ),
    })
}

fn exact_managed_raster_percent(
    name: &str,
    value: f64,
    minimum: f64,
    maximum: f64,
) -> AnyResult<i16> {
    if !value.is_finite() || value < minimum || value > maximum {
        bail!("managed raster mask {name} must be finite and in [{minimum}, {maximum}]");
    }
    let scaled = value * 100.0;
    let rounded = scaled.round();
    if (scaled - rounded).abs() > 1.0e-7 {
        bail!("managed raster mask {name} must use exact one-percent increments");
    }
    if rounded < f64::from(i16::MIN) || rounded > f64::from(i16::MAX) {
        bail!("managed raster mask {name} percentage exceeds i16");
    }
    // Finiteness, integrality, and the complete i16 range are checked above.
    #[allow(clippy::cast_possible_truncation)]
    let exact = rounded as i16;
    Ok(exact)
}

// The tagged FFI mask union is decoded exhaustively in one place so every
// variant shares the same validation and managed-raster preservation policy.
#[allow(clippy::too_many_lines)]
fn local_mask_definition_from_ffi(
    grade_node: &ffi::FfiGradeNode,
    index: usize,
) -> AnyResult<(
    Option<MaskDefinition>,
    Option<PreservedManagedRasterSettings>,
)> {
    let unit = |name: &str, value: f64| {
        UnitInterval::new(value)
            .with_context(|| format!("Grade Node {index} local mask {name} must be in [0, 1]"))
    };
    match grade_node.local_mask_kind {
        LOCAL_MASK_NONE => Ok((None, None)),
        LOCAL_MASK_LINEAR_GRADIENT => Ok((
            Some(MaskDefinition::linear_gradient(
                unit("start x", grade_node.local_mask_x0)?,
                unit("start y", grade_node.local_mask_y0)?,
                unit("end x", grade_node.local_mask_x1)?,
                unit("end y", grade_node.local_mask_y1)?,
                grade_node.local_mask_invert,
            )?),
            None,
        )),
        LOCAL_MASK_RADIAL_GRADIENT => Ok((
            Some(MaskDefinition::radial_gradient(
                unit("center x", grade_node.local_mask_x0)?,
                unit("center y", grade_node.local_mask_y0)?,
                unit("radius x", grade_node.local_mask_radius_x)?,
                unit("radius y", grade_node.local_mask_radius_y)?,
                unit("feather", grade_node.local_mask_feather)?,
                grade_node.local_mask_invert,
            )?),
            None,
        )),
        LOCAL_MASK_BRUSH => {
            if !grade_node.local_mask_brush_points.len().is_multiple_of(3) {
                bail!("Grade Node {index} brush mask must contain x/y/stroke triples");
            }
            let point_count = grade_node.local_mask_brush_points.len() / 3;
            if point_count > MAX_MASK_BRUSH_POINTS {
                bail!(
                    "Grade Node {index} brush mask contains {point_count} points, but at most {MAX_MASK_BRUSH_POINTS} are supported"
                );
            }
            let mut points = Vec::with_capacity(point_count);
            for (point_index, point) in grade_node
                .local_mask_brush_points
                .chunks_exact(3)
                .enumerate()
            {
                let begins_stroke = match point[2] {
                    0.0 => false,
                    1.0 => true,
                    _ => {
                        bail!(
                            "Grade Node {index} brush point {point_index} has an invalid stroke marker"
                        )
                    }
                };
                points.push(MaskBrushPoint::new(
                    unit("brush x", point[0])?,
                    unit("brush y", point[1])?,
                    begins_stroke,
                ));
            }
            Ok((
                Some(MaskDefinition::brush(
                    points,
                    unit("brush radius", grade_node.local_mask_radius_x)?,
                    unit("brush feather", grade_node.local_mask_feather)?,
                    grade_node.local_mask_invert,
                )?),
                None,
            ))
        }
        LOCAL_MASK_LUMINANCE_RANGE => Ok((
            Some(MaskDefinition::luminance_range(
                unit("luminance lower bound", grade_node.local_mask_x0)?,
                unit("luminance upper bound", grade_node.local_mask_x1)?,
                unit("luminance softness", grade_node.local_mask_feather)?,
                grade_node.local_mask_invert,
            )?),
            None,
        )),
        LOCAL_MASK_COLOR_RANGE => Ok((
            Some(MaskDefinition::color_range(
                unit("color center", grade_node.local_mask_x0)?.get() * 360.0,
                unit("color width", grade_node.local_mask_x1)?.get() * 180.0,
                unit("color softness", grade_node.local_mask_feather)?,
                grade_node.local_mask_invert,
            )?),
            None,
        )),
        LOCAL_MASK_MANAGED_RASTER => {
            let expansion_percent =
                exact_managed_raster_percent("expansion", grade_node.local_mask_x0, -1.0, 1.0)?;
            let feather_percent =
                exact_managed_raster_percent("feather", grade_node.local_mask_feather, 0.0, 1.0)?;
            Ok((
                None,
                Some(PreservedManagedRasterSettings {
                    expansion_percent: i8::try_from(expansion_percent)
                        .context("managed raster expansion percentage exceeds i8")?,
                    feather_percent: u8::try_from(feather_percent)
                        .context("managed raster feather percentage exceeds u8")?,
                    invert: grade_node.local_mask_invert,
                }),
            ))
        }
        other => bail!("Grade Node {index} has unsupported local mask kind {other}"),
    }
}

fn photo_geometry_from_ffi(geometry: &ffi::FfiPhotoGeometry) -> AnyResult<PhotoGeometry> {
    let unit = |name: &str, value: f64| {
        UnitInterval::new(value).with_context(|| format!("photo geometry {name} must be in [0, 1]"))
    };
    let quarter_turn = match geometry.quarter_turn {
        0 => PhotoQuarterTurn::Zero,
        1 => PhotoQuarterTurn::Clockwise90,
        2 => PhotoQuarterTurn::Clockwise180,
        3 => PhotoQuarterTurn::Clockwise270,
        other => bail!("photo geometry uses unsupported quarter-turn {other}"),
    };
    PhotoGeometry::new(
        unit("crop left", geometry.crop_left)?,
        unit("crop top", geometry.crop_top)?,
        unit("crop right", geometry.crop_right)?,
        unit("crop bottom", geometry.crop_bottom)?,
        quarter_turn,
        geometry.flip_horizontal,
        geometry.flip_vertical,
    )
    .and_then(|value| value.with_straighten_degrees(geometry.straighten_degrees))
    .map_err(Into::into)
}

fn photo_canvas_from_ffi(geometry: &ffi::FfiPhotoGeometry) -> AnyResult<PhotoCanvasNode> {
    let canvas = PhotoCanvasNode::new(photo_geometry_from_ffi(geometry)?)
        .with_present(geometry.present)
        .with_enabled(geometry.enabled);
    Ok(canvas)
}

fn ffi_photo_canvas(canvas: PhotoCanvasNode) -> ffi::FfiPhotoGeometry {
    let geometry = canvas.geometry();
    ffi::FfiPhotoGeometry {
        present: canvas.is_present(),
        enabled: canvas.enabled(),
        crop_left: geometry.crop_left().get(),
        crop_top: geometry.crop_top().get(),
        crop_right: geometry.crop_right().get(),
        crop_bottom: geometry.crop_bottom().get(),
        quarter_turn: match geometry.quarter_turn() {
            PhotoQuarterTurn::Zero => 0,
            PhotoQuarterTurn::Clockwise90 => 1,
            PhotoQuarterTurn::Clockwise180 => 2,
            PhotoQuarterTurn::Clockwise270 => 3,
        },
        straighten_degrees: geometry.straighten_degrees(),
        flip_horizontal: geometry.flip_horizontal(),
        flip_vertical: geometry.flip_vertical(),
    }
}

pub(super) fn adjustment_geometry(geometry: PhotoGeometry) -> AdjustmentGeometry {
    AdjustmentGeometry {
        crop_left: geometry.crop_left().get(),
        crop_top: geometry.crop_top().get(),
        crop_right: geometry.crop_right().get(),
        crop_bottom: geometry.crop_bottom().get(),
        quarter_turn: match geometry.quarter_turn() {
            PhotoQuarterTurn::Zero => AdjustmentQuarterTurn::Zero,
            PhotoQuarterTurn::Clockwise90 => AdjustmentQuarterTurn::Clockwise90,
            PhotoQuarterTurn::Clockwise180 => AdjustmentQuarterTurn::Clockwise180,
            PhotoQuarterTurn::Clockwise270 => AdjustmentQuarterTurn::Clockwise270,
        },
        straighten_degrees: geometry.straighten_degrees(),
        flip_horizontal: geometry.flip_horizontal(),
        flip_vertical: geometry.flip_vertical(),
    }
}

pub(crate) fn recipe_optics_settings(settings: &ffi::FfiOpticsSettings) -> RecipeOpticsSettings {
    RecipeOpticsSettings::new(
        settings.enabled,
        settings.correct_distortion,
        settings.correct_tca,
        settings.correct_vignetting,
        settings.automatic_scale,
    )
    .with_manual_corrections(
        settings.manual_distortion,
        settings.manual_tca_red_cyan,
        settings.manual_tca_blue_yellow,
        settings.manual_vignetting_amount,
        settings.manual_vignetting_midpoint,
    )
    .with_manual_profile(
        settings.camera_profile_maker.clone(),
        settings.camera_profile_model.clone(),
        settings.lens_profile_maker.clone(),
        settings.lens_profile_model.clone(),
    )
}

pub(crate) fn ffi_optics_settings(settings: &RecipeOpticsSettings) -> ffi::FfiOpticsSettings {
    ffi::FfiOpticsSettings {
        enabled: settings.enabled(),
        correct_distortion: settings.correct_distortion(),
        correct_tca: settings.correct_tca(),
        correct_vignetting: settings.correct_vignetting(),
        automatic_scale: settings.automatic_scale(),
        manual_distortion: settings.manual_distortion(),
        manual_tca_red_cyan: settings.manual_tca_red_cyan(),
        manual_tca_blue_yellow: settings.manual_tca_blue_yellow(),
        manual_vignetting_amount: settings.manual_vignetting_amount(),
        manual_vignetting_midpoint: settings.manual_vignetting_midpoint(),
        camera_profile_maker: settings.camera_profile_maker().to_owned(),
        camera_profile_model: settings.camera_profile_model().to_owned(),
        lens_profile_maker: settings.lens_profile_maker().to_owned(),
        lens_profile_model: settings.lens_profile_model().to_owned(),
    }
}

fn raw_white_balance_from_ffi(
    foundation: &ffi::FfiPhotoFoundationSettings,
) -> AnyResult<RawWhiteBalance> {
    match foundation.raw_white_balance_mode {
        RAW_WHITE_BALANCE_AS_SHOT => Ok(RawWhiteBalance::AsShot),
        RAW_WHITE_BALANCE_TEMPERATURE_TINT => Ok(RawWhiteBalance::temperature_tint(
            RawTemperatureTint::new(foundation.temperature_kelvin, foundation.tint)
                .context("RAW Foundation temperature/tint")?,
        )),
        other => bail!("RAW Foundation has unsupported white-balance mode {other}"),
    }
}

fn raw_ai_denoise_from_ffi(
    foundation: &ffi::FfiPhotoFoundationSettings,
) -> AnyResult<RawFoundationDenoise> {
    let model = match foundation.raw_ai_denoise_model {
        RAW_AI_DENOISE_MODEL_RAWNIND_PUBLIC_BAYER_RELEASE_5_6_0 => {
            RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0
        }
        other => bail!("RAW Foundation has unsupported AI denoise model {other}"),
    };
    RawFoundationDenoise::added(model)
        .with_present(foundation.raw_ai_denoise_present)
        .with_enabled(foundation.raw_ai_denoise_enabled)
        .with_bypassed(foundation.raw_ai_denoise_bypassed)
        .with_amount_percent(foundation.raw_ai_denoise_amount_percent)
        .context("AI RAW denoise amount")
}

pub(crate) fn ffi_photo_foundation_settings(
    foundation: &PhotoFoundationNode,
    raw_ai_denoise: RawFoundationDenoise,
) -> ffi::FfiPhotoFoundationSettings {
    let (raw_white_balance_mode, temperature_kelvin, tint) = match foundation.raw_white_balance() {
        RawWhiteBalance::AsShot => (
            RAW_WHITE_BALANCE_AS_SHOT,
            RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN,
            0,
        ),
        RawWhiteBalance::TemperatureTint { value } => (
            RAW_WHITE_BALANCE_TEMPERATURE_TINT,
            value.temperature_kelvin(),
            value.tint(),
        ),
    };
    let raw_ai_denoise_model = match raw_ai_denoise.model() {
        RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0 => {
            RAW_AI_DENOISE_MODEL_RAWNIND_PUBLIC_BAYER_RELEASE_5_6_0
        }
    };
    ffi::FfiPhotoFoundationSettings {
        enabled: foundation.enabled(),
        optics: ffi_optics_settings(foundation.optics()),
        raw_ai_denoise_present: raw_ai_denoise.is_present(),
        raw_ai_denoise_enabled: raw_ai_denoise.is_enabled(),
        raw_ai_denoise_bypassed: raw_ai_denoise.is_bypassed(),
        raw_ai_denoise_model,
        raw_ai_denoise_amount_percent: raw_ai_denoise.amount_percent(),
        raw_white_balance_mode,
        temperature_kelvin,
        tint,
        as_shot_white_balance_available: false,
        as_shot_temperature_kelvin: RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN,
        as_shot_tint: 0,
    }
}

pub(crate) fn new_basic_grade_node(label: &str) -> AnyResult<ffi::FfiGradeNode> {
    let grade_node = GradeNodeDraft::neutral(label);
    let grade_stack = GradeStackDraft {
        raw_ai_denoise: RawFoundationDenoise::default(),
        foundation: PhotoFoundationNode::default(),
        grade_nodes: vec![grade_node.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        liquify: None,
        canvas: PhotoCanvasNode::identity(),
    };
    grade_stack_recipe_v1_snapshot(&grade_stack, None).context("validate new Basic Grade Node")?;
    encode_grade_node_draft_recipe_v1(grade_node)
}

pub(crate) fn decode_grade_stack_draft_recipe_v1(
    settings: &ffi::FfiEditSettings,
) -> AnyResult<GradeStackDraft> {
    if !(1..=MAX_GRADE_NODES).contains(&settings.grade_nodes.len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    let grade_stack = GradeStackDraft {
        raw_ai_denoise: raw_ai_denoise_from_ffi(&settings.foundation)?,
        foundation: PhotoFoundationNode::new(
            RecipeInputSettings::new(recipe_optics_settings(&settings.foundation.optics))
                .with_enabled(settings.foundation.enabled)
                .with_raw_white_balance(raw_white_balance_from_ffi(&settings.foundation)?),
        ),
        grade_nodes: settings
            .grade_nodes
            .iter()
            .enumerate()
            .map(|(index, grade_node)| decode_grade_node_draft_recipe_v1(grade_node, index))
            .collect::<AnyResult<Vec<_>>>()?,
        retouch_spots: settings
            .retouch_spots
            .iter()
            .enumerate()
            .map(|(index, spot)| {
                let mode = match spot.mode {
                    0 => RetouchMode::Heal,
                    1 => RetouchMode::Clone,
                    other => bail!("retouch spot {index} has unsupported mode {other}"),
                };
                RetouchSpot::new(
                    UnitInterval::new(spot.center_x).with_context(|| {
                        format!("retouch spot {index} center x must be in [0, 1]")
                    })?,
                    UnitInterval::new(spot.center_y).with_context(|| {
                        format!("retouch spot {index} center y must be in [0, 1]")
                    })?,
                    spot.radius_level_zero_pixels,
                )
                .and_then(|value| {
                    value.with_behavior(
                        mode,
                        spot.source_offset_x_radii,
                        spot.source_offset_y_radii,
                        UnitInterval::new(spot.feather)?,
                    )
                })
                .and_then(|value| Ok(value.with_strength(UnitInterval::new(spot.strength)?)))
                .with_context(|| format!("retouch spot {index} is invalid"))
            })
            .collect::<AnyResult<Vec<_>>>()?,
        retouch_strokes: settings
            .retouch_strokes
            .iter()
            .enumerate()
            .map(|(index, stroke)| {
                let mode = match stroke.mode {
                    0 => RetouchMode::Heal,
                    1 => RetouchMode::Clone,
                    other => bail!("retouch stroke {index} has unsupported mode {other}"),
                };
                let points = stroke
                    .points
                    .iter()
                    .enumerate()
                    .map(|(point_index, point)| {
                        Ok(RetouchPoint::new(
                            UnitInterval::new(point.x).with_context(|| {
                                format!(
                                    "retouch stroke {index} point {point_index} x must be in [0, 1]"
                                )
                            })?,
                            UnitInterval::new(point.y).with_context(|| {
                                format!(
                                    "retouch stroke {index} point {point_index} y must be in [0, 1]"
                                )
                            })?,
                        ))
                    })
                    .collect::<AnyResult<Vec<_>>>()?;
                RetouchStroke::new(points, stroke.radius_level_zero_pixels)
                    .and_then(|value| {
                        value.with_behavior(
                            mode,
                            stroke.source_offset_x_radii,
                            stroke.source_offset_y_radii,
                            UnitInterval::new(stroke.feather)?,
                        )
                    })
                    .and_then(|value| Ok(value.with_strength(UnitInterval::new(stroke.strength)?)))
                    .with_context(|| format!("retouch stroke {index} is invalid"))
            })
            .collect::<AnyResult<Vec<_>>>()?,
        liquify: photo_liquify_from_ffi(&settings.liquify_strokes, settings.liquify_enabled)?,
        canvas: photo_canvas_from_ffi(&settings.geometry)?,
    };
    validate_grade_stack_draft_recipe_v1(&grade_stack)?;
    // Domain construction authoritatively validates labels and the complete
    // graph generated from the untrusted desktop DTO. An opaque managed mask
    // has no authority without its explicit base Recipe, so omit only that
    // marker from this preliminary graph check; snapshot encoding later must
    // materialize it from the supplied base before rendering or persistence.
    let mut graph_validation = grade_stack.clone();
    for grade_node in &mut graph_validation.grade_nodes {
        grade_node.preserved_managed_raster = None;
    }
    grade_stack_recipe_v1_snapshot(&graph_validation, None)
        .context("validate Grade Stack Recipe v1")?;
    Ok(grade_stack)
}

pub(crate) fn decode_grade_node_draft_recipe_v1(
    grade_node: &ffi::FfiGradeNode,
    index: usize,
) -> AnyResult<GradeNodeDraft> {
    let parse_grade_node_id = |value: &str| {
        value
            .parse::<LayerInstanceId>()
            .with_context(|| format!("parse Grade Node {index} id {value:?}"))
    };
    let parse_render_op_id = |role: &str, value: &str| {
        value.parse::<NodeId>().with_context(|| {
            format!("parse Grade Node {index} Recipe v1 {role} render-op id {value:?}")
        })
    };
    let grade_node_id = parse_grade_node_id(&grade_node.grade_node_id)?;
    let shared = match (
        grade_node.shared_layer_id.is_empty(),
        grade_node.shared_revision_id.is_empty(),
    ) {
        (true, true) => None,
        (false, false) => Some(SharedGradeNodeReference {
            layer_id: grade_node
                .shared_layer_id
                .parse::<LayerId>()
                .with_context(|| {
                    format!(
                        "parse Grade Node {index} shared layer id {:?}",
                        grade_node.shared_layer_id
                    )
                })?,
            revision_id: grade_node
                .shared_revision_id
                .parse::<LayerRevisionId>()
                .with_context(|| {
                    format!(
                        "parse Grade Node {index} shared revision id {:?}",
                        grade_node.shared_revision_id
                    )
                })?,
        }),
        _ => bail!("Grade Node {index} has an incomplete shared-node reference"),
    };
    let (local_mask, preserved_managed_raster) = local_mask_definition_from_ffi(grade_node, index)?;
    Ok(GradeNodeDraft {
        recipe_v1_identity: GradeNodeRecipeV1Identity {
            grade_node_id,
            exposure_render_op_id: parse_render_op_id(
                "exposure",
                &grade_node.exposure_render_op_id,
            )?,
            contrast_render_op_id: parse_render_op_id(
                "contrast",
                &grade_node.contrast_render_op_id,
            )?,
            oklab_lightness_curve_render_op_id: recipe_v1_oklab_lightness_tone_curve_render_op_id(
                grade_node_id,
            ),
            selective_tone_render_op_id: parse_render_op_id(
                "selective tone",
                &grade_node.selective_tone_render_op_id,
            )?,
            white_balance_render_op_id: parse_render_op_id(
                "channel gain",
                &grade_node.white_balance_render_op_id,
            )?,
            saturation_render_op_id: parse_render_op_id(
                "saturation",
                &grade_node.saturation_render_op_id,
            )?,
            perceptual_color_render_op_id: parse_render_op_id(
                "perceptual color",
                &grade_node.perceptual_color_render_op_id,
            )?,
            oklab_color_warper_render_op_id: recipe_v1_oklab_color_warper_render_op_id(
                grade_node_id,
            ),
            lut_render_op_id: parse_render_op_id("LUT", &grade_node.lut_render_op_id)?,
            sharpen_render_op_id: parse_render_op_id("sharpen", &grade_node.sharpen_render_op_id)?,
            color_grading_render_op_id: recipe_color_grading_render_op_id(grade_node_id),
            finishing_effects_render_op_id: recipe_finishing_effects_render_op_id(grade_node_id),
        },
        shared,
        local_mask,
        preserved_managed_raster,
        label: grade_node.label.clone(),
        opacity: UnitInterval::new(grade_node.opacity)
            .with_context(|| format!("validate Grade Node {index} opacity"))?,
        basic: basic_parameters(&grade_node.basic)?,
        fine: fine_parameters(&grade_node.fine)?,
        enabled: grade_node.enabled,
    })
}

pub(crate) fn fixed_color_mixer(
    values: &[f64],
    name: &str,
) -> AnyResult<[f64; COLOR_MIXER_BAND_COUNT]> {
    values.try_into().map_err(|_| {
        anyhow!("{name} must contain exactly {COLOR_MIXER_BAND_COUNT} hue-band values")
    })
}

pub(crate) fn fixed_selective_color(
    values: &[f64],
) -> AnyResult<[f64; SELECTIVE_COLOR_VALUE_COUNT]> {
    values.try_into().map_err(|_| {
        anyhow!(
            "selective_color_cmyk must contain exactly {SELECTIVE_COLOR_VALUE_COUNT} CMYK values"
        )
    })
}

pub(crate) fn oklab_color_warper_from_ffi(
    values: &[f64],
    strength: f64,
) -> AnyResult<OklabColorWarperParameters> {
    if values.len() != OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2 {
        bail!(
            "oklab_color_warper_control_points must contain exactly {} a/b values",
            OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2
        );
    }
    let mut control_points = [OklabColorWarperControlPoint {
        a_offset: 0.0,
        b_offset: 0.0,
    }; OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT];
    for (index, point) in control_points.iter_mut().enumerate() {
        point.a_offset = values[index * 2];
        point.b_offset = values[index * 2 + 1];
    }
    Ok(OklabColorWarperParameters {
        control_points,
        strength,
    })
}

pub(crate) fn oklab_color_warper_ffi_values(parameters: &OklabColorWarperParameters) -> Vec<f64> {
    parameters
        .control_points
        .iter()
        .flat_map(|point| [point.a_offset, point.b_offset])
        .collect()
}

pub(crate) fn fine_parameters(
    parameters: &ffi::FfiFineEditParameters,
) -> AnyResult<FineEditParameters> {
    let parameters = FineEditParameters {
        selective_tone: SelectiveToneParameters {
            highlights: parameters.highlights,
            shadows: parameters.shadows,
            whites: parameters.whites,
            blacks: parameters.blacks,
        },
        perceptual_color: PerceptualColorParameters {
            global_a_balance: parameters.global_a_balance,
            global_b_balance: parameters.global_b_balance,
            vibrance: parameters.vibrance,
            hue_shifts: fixed_color_mixer(&parameters.mixer_hue, "mixer_hue")?,
            saturation: fixed_color_mixer(&parameters.mixer_saturation, "mixer_saturation")?,
            lightness: fixed_color_mixer(&parameters.mixer_lightness, "mixer_lightness")?,
            color_range: ColorRangeParameters {
                enabled: parameters.color_range_enabled,
                center_hue_degrees: parameters.color_range_center,
                width_degrees: parameters.color_range_width,
                softness: parameters.color_range_softness,
                hue_shift_degrees: parameters.color_range_hue,
                saturation: parameters.color_range_saturation,
                lightness: parameters.color_range_lightness,
            },
            additional_color_ranges: point_color_ranges_from_vector_optional(
                &parameters.point_color_ranges,
            )?,
            selective_color_relative: parameters.selective_color_relative,
            selective_color_lightness_protection: parameters.selective_color_lightness_protection,
            selective_color_cmyk: fixed_selective_color(&parameters.selective_color_cmyk)?,
        },
        oklab_color_warper: oklab_color_warper_from_ffi(
            &parameters.oklab_color_warper_control_points,
            parameters.oklab_color_warper_strength,
        )?,
        oklab_lightness_curve: if parameters.oklab_lightness_curve_points.is_empty() {
            None
        } else {
            Some(OklabLightnessToneCurve {
                lightness: tone_curve_points_from_vector(&parameters.oklab_lightness_curve_points)?,
            })
        },
        lut: LutEditParameters {
            resource_id: parameters.lut_resource_id.clone(),
            title: parameters.lut_title.clone(),
            managed_path: parameters.lut_managed_path.clone(),
            intensity: parameters.lut_intensity,
        },
        sharpen: SharpenParameters {
            amount: parameters.sharpen_amount,
            radius: parameters.sharpen_radius,
            threshold: parameters.sharpen_threshold,
            masking: parameters.sharpen_masking,
            clarity: parameters.clarity,
            texture: parameters.texture,
            local_contrast: parameters.local_contrast,
            local_contrast_scale: parameters.local_contrast_scale,
            denoise_luminance: parameters.denoise_luminance,
            denoise_detail: parameters.denoise_detail,
            denoise_color: parameters.denoise_color,
            dehaze: parameters.dehaze,
            defringe_purple_amount: parameters.defringe_purple_amount,
            defringe_purple_hue_low: parameters.defringe_purple_hue_low,
            defringe_purple_hue_high: parameters.defringe_purple_hue_high,
            defringe_green_amount: parameters.defringe_green_amount,
            defringe_green_hue_low: parameters.defringe_green_hue_low,
            defringe_green_hue_high: parameters.defringe_green_hue_high,
            shadows_hue: parameters.shadows_hue,
            shadows_saturation: parameters.shadows_saturation,
            shadows_luminance: parameters.shadows_luminance,
            midtones_hue: parameters.midtones_hue,
            midtones_saturation: parameters.midtones_saturation,
            midtones_luminance: parameters.midtones_luminance,
            highlights_hue: parameters.highlights_hue,
            highlights_saturation: parameters.highlights_saturation,
            highlights_luminance: parameters.highlights_luminance,
            grading_blending: parameters.grading_blending,
            grading_balance: parameters.grading_balance,
            grain_amount: parameters.grain_amount,
            grain_size: parameters.grain_size,
            grain_roughness: parameters.grain_roughness,
            vignette_amount: parameters.vignette_amount,
            vignette_midpoint: parameters.vignette_midpoint,
            vignette_roundness: parameters.vignette_roundness,
            vignette_feather: parameters.vignette_feather,
            vignette_highlights: parameters.vignette_highlights,
        },
    };
    validate_fine_parameters(&parameters)?;
    Ok(parameters)
}

pub(crate) fn basic_parameters(
    parameters: &ffi::FfiBasicEditParameters,
) -> AnyResult<BasicEditParameters> {
    let parameters = BasicEditParameters {
        exposure_stops: parameters.exposure_stops,
        contrast_factor: parameters.contrast_factor,
        white_balance_temperature: parameters.white_balance_temperature,
        white_balance_tint: parameters.white_balance_tint,
        saturation_factor: parameters.saturation_factor,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
}

pub(crate) fn preview_grade_stack_draft_recipe_v1(
    settings: &ffi::FfiEditSettings,
    use_working_recipe: bool,
) -> AnyResult<GradeStackDraft> {
    if use_working_recipe {
        decode_grade_stack_draft_recipe_v1(settings)
    } else {
        // Before is a product-level neutral import baseline, not merely a
        // render that happens to omit the persisted working Recipe. Both the
        // current slider state and Tone Curve must be excluded.
        Ok(GradeStackDraft::default())
    }
}

pub(crate) fn ffi_basic_parameters(parameters: BasicEditParameters) -> ffi::FfiBasicEditParameters {
    ffi::FfiBasicEditParameters {
        exposure_stops: parameters.exposure_stops,
        contrast_factor: parameters.contrast_factor,
        white_balance_temperature: parameters.white_balance_temperature,
        white_balance_tint: parameters.white_balance_tint,
        saturation_factor: parameters.saturation_factor,
    }
}

pub(crate) fn ffi_fine_parameters(parameters: &FineEditParameters) -> ffi::FfiFineEditParameters {
    let tone = parameters.selective_tone;
    let color = &parameters.perceptual_color;
    let range = color.color_range;
    ffi::FfiFineEditParameters {
        highlights: tone.highlights,
        shadows: tone.shadows,
        whites: tone.whites,
        blacks: tone.blacks,
        global_a_balance: color.global_a_balance,
        global_b_balance: color.global_b_balance,
        vibrance: color.vibrance,
        mixer_hue: color.hue_shifts.to_vec(),
        mixer_saturation: color.saturation.to_vec(),
        mixer_lightness: color.lightness.to_vec(),
        color_range_enabled: range.enabled,
        color_range_center: range.center_hue_degrees,
        color_range_width: range.width_degrees,
        color_range_softness: range.softness,
        color_range_hue: range.hue_shift_degrees,
        color_range_saturation: range.saturation,
        color_range_lightness: range.lightness,
        point_color_ranges: color
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
            .collect(),
        selective_color_relative: color.selective_color_relative,
        selective_color_lightness_protection: color.selective_color_lightness_protection,
        selective_color_cmyk: color.selective_color_cmyk.to_vec(),
        oklab_lightness_curve_points: parameters
            .oklab_lightness_curve
            .as_ref()
            .map(|curve| {
                curve
                    .lightness
                    .iter()
                    .flat_map(|point| [point.x, point.y])
                    .collect()
            })
            .unwrap_or_default(),
        oklab_color_warper_control_points: oklab_color_warper_ffi_values(
            &parameters.oklab_color_warper,
        ),
        oklab_color_warper_strength: parameters.oklab_color_warper.strength,
        lut_resource_id: parameters.lut.resource_id.clone(),
        lut_title: parameters.lut.title.clone(),
        lut_managed_path: parameters.lut.managed_path.clone(),
        lut_intensity: parameters.lut.intensity,
        sharpen_amount: parameters.sharpen.amount,
        sharpen_radius: parameters.sharpen.radius,
        sharpen_threshold: parameters.sharpen.threshold,
        sharpen_masking: parameters.sharpen.masking,
        clarity: parameters.sharpen.clarity,
        texture: parameters.sharpen.texture,
        local_contrast: parameters.sharpen.local_contrast,
        local_contrast_scale: parameters.sharpen.local_contrast_scale,
        denoise_luminance: parameters.sharpen.denoise_luminance,
        denoise_detail: parameters.sharpen.denoise_detail,
        denoise_color: parameters.sharpen.denoise_color,
        dehaze: parameters.sharpen.dehaze,
        defringe_purple_amount: parameters.sharpen.defringe_purple_amount,
        defringe_purple_hue_low: parameters.sharpen.defringe_purple_hue_low,
        defringe_purple_hue_high: parameters.sharpen.defringe_purple_hue_high,
        defringe_green_amount: parameters.sharpen.defringe_green_amount,
        defringe_green_hue_low: parameters.sharpen.defringe_green_hue_low,
        defringe_green_hue_high: parameters.sharpen.defringe_green_hue_high,
        shadows_hue: parameters.sharpen.shadows_hue,
        shadows_saturation: parameters.sharpen.shadows_saturation,
        shadows_luminance: parameters.sharpen.shadows_luminance,
        midtones_hue: parameters.sharpen.midtones_hue,
        midtones_saturation: parameters.sharpen.midtones_saturation,
        midtones_luminance: parameters.sharpen.midtones_luminance,
        highlights_hue: parameters.sharpen.highlights_hue,
        highlights_saturation: parameters.sharpen.highlights_saturation,
        highlights_luminance: parameters.sharpen.highlights_luminance,
        grading_blending: parameters.sharpen.grading_blending,
        grading_balance: parameters.sharpen.grading_balance,
        grain_amount: parameters.sharpen.grain_amount,
        grain_size: parameters.sharpen.grain_size,
        grain_roughness: parameters.sharpen.grain_roughness,
        vignette_amount: parameters.sharpen.vignette_amount,
        vignette_midpoint: parameters.sharpen.vignette_midpoint,
        vignette_roundness: parameters.sharpen.vignette_roundness,
        vignette_feather: parameters.sharpen.vignette_feather,
        vignette_highlights: parameters.sharpen.vignette_highlights,
    }
}

pub(crate) fn encode_grade_stack_draft_recipe_v1(
    grade_stack: GradeStackDraft,
) -> AnyResult<ffi::FfiEditSettings> {
    let (liquify_enabled, liquify_strokes) = grade_stack.liquify.map_or_else(
        || (false, Vec::new()),
        |node| {
            let strokes = node
                .strokes()
                .iter()
                .map(|stroke| ffi::FfiLiquifyStroke {
                    kind: u8::from(stroke.is_reconstruct()),
                    points: stroke
                        .points()
                        .iter()
                        .map(|point| ffi::FfiLiquifyPoint {
                            x: point.x().get(),
                            y: point.y().get(),
                            pressure: point.pressure().get(),
                        })
                        .collect(),
                    radius: stroke.radius().get(),
                    strength: stroke.strength().get(),
                    hardness: stroke.hardness().get(),
                })
                .collect();
            (node.enabled(), strokes)
        },
    );
    Ok(ffi::FfiEditSettings {
        foundation: ffi_photo_foundation_settings(
            &grade_stack.foundation,
            grade_stack.raw_ai_denoise,
        ),
        grade_nodes: grade_stack
            .grade_nodes
            .into_iter()
            .map(encode_grade_node_draft_recipe_v1)
            .collect::<AnyResult<Vec<_>>>()?,
        retouch_spots: grade_stack
            .retouch_spots
            .into_iter()
            .map(|spot| ffi::FfiRetouchSpot {
                center_x: spot.center_x().get(),
                center_y: spot.center_y().get(),
                radius_level_zero_pixels: spot.radius_level_zero_pixels(),
                mode: match spot.mode() {
                    RetouchMode::Heal => 0,
                    RetouchMode::Clone => 1,
                },
                source_offset_x_radii: spot.source_offset_x_radii(),
                source_offset_y_radii: spot.source_offset_y_radii(),
                feather: spot.feather().get(),
                strength: spot.strength().get(),
            })
            .collect(),
        retouch_strokes: grade_stack
            .retouch_strokes
            .into_iter()
            .map(|stroke| ffi::FfiRetouchStroke {
                points: stroke
                    .points()
                    .iter()
                    .map(|point| ffi::FfiRetouchPoint {
                        x: point.x().get(),
                        y: point.y().get(),
                    })
                    .collect(),
                radius_level_zero_pixels: stroke.radius_level_zero_pixels(),
                mode: match stroke.mode() {
                    RetouchMode::Heal => 0,
                    RetouchMode::Clone => 1,
                },
                source_offset_x_radii: stroke.source_offset_x_radii(),
                source_offset_y_radii: stroke.source_offset_y_radii(),
                feather: stroke.feather().get(),
                strength: stroke.strength().get(),
            })
            .collect(),
        liquify_enabled,
        liquify_strokes,
        geometry: ffi_photo_canvas(grade_stack.canvas),
    })
}

// The x/y names mirror the stable FFI schema; renaming only one side would
// make the projection harder to audit than the intentional similarity.
#[allow(clippy::similar_names)]
pub(crate) fn encode_grade_node_draft_recipe_v1(
    grade_node: GradeNodeDraft,
) -> AnyResult<ffi::FfiGradeNode> {
    let identity = grade_node.recipe_v1_identity;
    let (shared_layer_id, shared_revision_id) = grade_node.shared.map_or_else(
        || (String::new(), String::new()),
        |shared| (shared.layer_id.to_string(), shared.revision_id.to_string()),
    );
    let (
        local_mask_kind,
        local_mask_x0,
        local_mask_y0,
        local_mask_x1,
        local_mask_y1,
        local_mask_radius_x,
        local_mask_radius_y,
        local_mask_feather,
        local_mask_invert,
        local_mask_brush_points,
    ) = ffi_local_mask_fields(
        grade_node.local_mask.as_ref(),
        grade_node.preserved_managed_raster,
    )?;
    Ok(ffi::FfiGradeNode {
        grade_node_id: identity.grade_node_id.to_string(),
        shared_layer_id,
        shared_revision_id,
        local_mask_kind,
        local_mask_x0,
        local_mask_y0,
        local_mask_x1,
        local_mask_y1,
        local_mask_radius_x,
        local_mask_radius_y,
        local_mask_feather,
        local_mask_invert,
        local_mask_brush_points,
        label: grade_node.label,
        opacity: grade_node.opacity.get(),
        enabled: grade_node.enabled,
        exposure_render_op_id: identity.exposure_render_op_id.to_string(),
        contrast_render_op_id: identity.contrast_render_op_id.to_string(),
        selective_tone_render_op_id: identity.selective_tone_render_op_id.to_string(),
        white_balance_render_op_id: identity.white_balance_render_op_id.to_string(),
        saturation_render_op_id: identity.saturation_render_op_id.to_string(),
        perceptual_color_render_op_id: identity.perceptual_color_render_op_id.to_string(),
        lut_render_op_id: identity.lut_render_op_id.to_string(),
        sharpen_render_op_id: identity.sharpen_render_op_id.to_string(),
        basic: ffi_basic_parameters(grade_node.basic),
        fine: ffi_fine_parameters(&grade_node.fine),
    })
}

#[cfg(test)]
mod tests;

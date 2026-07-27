//! Recipe v1's complete desktop adapter contract.
//!
//! This module owns the editable Grade Stack draft, validation, immutable
//! RecipeSnapshot serialization, reverse decoding, and compilation into the
//! typed image render plan. The desktop session remains only the orchestration
//! facade around this contract.

use super::*;

mod compiler;
mod snapshot_decode;
mod snapshot_encode;

pub(crate) use compiler::*;
pub(crate) use snapshot_decode::*;
pub(crate) use snapshot_encode::*;

const _: () = assert!(
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION == ADJUSTMENT_PARAMETER_SCHEMA_VERSION
        && CPU_REFERENCE_IMPLEMENTATION_REVISION == ADJUSTMENT_IMPLEMENTATION_VERSION
        && OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION
            == OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_REVISION
        && SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION == SELECTIVE_TONE_PARAMETER_SCHEMA_REVISION
);

pub(crate) const MAX_GRADE_NODES: usize = 16;
pub(crate) const CONTRAST_PIVOT: f64 = 0.18;
pub(crate) const RECIPE_V1_OKLAB_LIGHTNESS_TONE_CURVE_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.oklab-lightness-tone-curve-slot-id.v1\0";
pub(crate) const RECIPE_V1_SELECTIVE_TONE_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.selective-tone-slot-id.v1\0";
pub(crate) const RECIPE_V1_PERCEPTUAL_COLOR_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.perceptual-color-slot-id.v1\0";
pub(crate) const RECIPE_V1_OKLAB_COLOR_WARPER_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.oklab-color-warper-slot-id.v1\0";
pub(crate) const RECIPE_V1_LUT_RENDER_OP_ID_DOMAIN: &[u8] = b"shadow.desktop.lut-slot-id.v1\0";
/// A local spatial mask belongs to the *instance* of a Grade Node. Its
/// generated identity incorporates the shape payload, so two immutable recipe
/// snapshots can never claim that one mask revision means different pixels.
pub(crate) const RECIPE_V1_LOCAL_MASK_ID_DOMAIN: &[u8] =
    b"shadow.desktop.local-mask-revision-id.v1\0";
// The external Qt DTO keeps its historical `sharpen_render_op_id` slot, which
// owns the technical-detail role. The two additional internal slots are
// deterministic from the Grade Node identity and never leak as extra UI controls.
pub(crate) const RECIPE_V1_TECHNICAL_DETAIL_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.technical-detail-slot-id.v1\0";
pub(crate) const RECIPE_V1_COLOR_GRADING_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.color-grading-slot-id.v1\0";
pub(crate) const RECIPE_V1_FINISHING_EFFECTS_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.finishing-effects-slot-id.v1\0";

/// Derives the reserved identity of a render-operation slot from its owning
/// Grade Node. UUID version 8 marks this as a Shadow-defined value while the
/// RFC 4122 variant keeps it interoperable with the typed UUID wrappers.
pub(crate) fn recipe_v1_derived_render_op_id(
    domain: &[u8],
    grade_node_id: LayerInstanceId,
) -> NodeId {
    let mut hasher = blake3::Hasher::new();
    hasher.update(domain);
    hasher.update(grade_node_id.as_bytes());
    let mut bytes = [0_u8; 16];
    bytes.copy_from_slice(&hasher.finalize().as_bytes()[..16]);
    bytes[6] = (bytes[6] & 0x0f) | 0x80;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    NodeId::from_uuid(Uuid::from_bytes(bytes))
}

pub(crate) fn recipe_v1_oklab_lightness_tone_curve_render_op_id(
    grade_node_id: LayerInstanceId,
) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V1_OKLAB_LIGHTNESS_TONE_CURVE_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

pub(crate) fn recipe_v1_selective_tone_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V1_SELECTIVE_TONE_RENDER_OP_ID_DOMAIN, grade_node_id)
}

pub(crate) fn recipe_v1_perceptual_color_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V1_PERCEPTUAL_COLOR_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

pub(crate) fn recipe_v1_oklab_color_warper_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V1_OKLAB_COLOR_WARPER_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

pub(crate) fn recipe_v1_sharpen_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V1_TECHNICAL_DETAIL_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

pub(crate) fn recipe_color_grading_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V1_COLOR_GRADING_RENDER_OP_ID_DOMAIN, grade_node_id)
}

pub(crate) fn recipe_finishing_effects_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V1_FINISHING_EFFECTS_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

pub(crate) fn recipe_v1_lut_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V1_LUT_RENDER_OP_ID_DOMAIN, grade_node_id)
}

fn recipe_v1_local_mask_revision(
    grade_node_id: LayerInstanceId,
    definition: &MaskDefinition,
) -> AnyResult<MaskRevision> {
    let encoded = serde_json::to_vec(definition)
        .context("serialize normalized local-mask definition for identity")?;
    let mut hasher = blake3::Hasher::new();
    hasher.update(RECIPE_V1_LOCAL_MASK_ID_DOMAIN);
    hasher.update(grade_node_id.as_bytes());
    hasher.update(&encoded);
    let mut bytes = [0_u8; 16];
    bytes.copy_from_slice(&hasher.finalize().as_bytes()[..16]);
    bytes[6] = (bytes[6] & 0x0f) | 0x80;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    MaskRevision::new(
        MaskId::from_uuid(Uuid::from_bytes(bytes)),
        1,
        MaskCoordinateSpace::Original,
        definition.clone(),
    )
    .map_err(Into::into)
}

#[derive(Debug, Clone, PartialEq)]
#[allow(clippy::struct_field_names)]
pub(crate) struct GradeNodeRecipeV1Identity {
    pub(crate) grade_node_id: LayerInstanceId,
    pub(crate) exposure_render_op_id: NodeId,
    pub(crate) contrast_render_op_id: NodeId,
    pub(crate) oklab_lightness_curve_render_op_id: NodeId,
    pub(crate) selective_tone_render_op_id: NodeId,
    pub(crate) white_balance_render_op_id: NodeId,
    pub(crate) saturation_render_op_id: NodeId,
    pub(crate) perceptual_color_render_op_id: NodeId,
    pub(crate) oklab_color_warper_render_op_id: NodeId,
    pub(crate) lut_render_op_id: NodeId,
    pub(crate) color_grading_render_op_id: NodeId,
    pub(crate) sharpen_render_op_id: NodeId,
    pub(crate) finishing_effects_render_op_id: NodeId,
}

impl GradeNodeRecipeV1Identity {
    pub(crate) fn new() -> Self {
        let grade_node_id = LayerInstanceId::new_v7();
        Self {
            grade_node_id,
            exposure_render_op_id: NodeId::new_v7(),
            contrast_render_op_id: NodeId::new_v7(),
            oklab_lightness_curve_render_op_id: recipe_v1_oklab_lightness_tone_curve_render_op_id(
                grade_node_id,
            ),
            selective_tone_render_op_id: recipe_v1_selective_tone_render_op_id(grade_node_id),
            white_balance_render_op_id: NodeId::new_v7(),
            saturation_render_op_id: NodeId::new_v7(),
            perceptual_color_render_op_id: recipe_v1_perceptual_color_render_op_id(grade_node_id),
            oklab_color_warper_render_op_id: recipe_v1_oklab_color_warper_render_op_id(
                grade_node_id,
            ),
            lut_render_op_id: recipe_v1_lut_render_op_id(grade_node_id),
            sharpen_render_op_id: recipe_v1_sharpen_render_op_id(grade_node_id),
            color_grading_render_op_id: recipe_color_grading_render_op_id(grade_node_id),
            finishing_effects_render_op_id: recipe_finishing_effects_render_op_id(grade_node_id),
        }
    }

    /// Recipe v1 stores the controls inside one Grade Node as atomic
    /// `AdjustmentNode`s. These are compiler/adapter identities, not Grade
    /// Nodes exposed to the product surface.
    fn recipe_v1_render_op_ids(&self) -> [(&'static str, NodeId); 12] {
        [
            ("exposure", self.exposure_render_op_id),
            ("contrast", self.contrast_render_op_id),
            ("selective_tone", self.selective_tone_render_op_id),
            (
                "oklab_lightness_tone_curve",
                self.oklab_lightness_curve_render_op_id,
            ),
            ("rgb_white_balance", self.white_balance_render_op_id),
            ("saturation", self.saturation_render_op_id),
            ("perceptual_color", self.perceptual_color_render_op_id),
            ("oklab_color_warper", self.oklab_color_warper_render_op_id),
            ("color_grading", self.color_grading_render_op_id),
            ("lut", self.lut_render_op_id),
            ("technical_detail", self.sharpen_render_op_id),
            ("finishing_effects", self.finishing_effects_render_op_id),
        ]
    }

    #[cfg(test)]
    pub(crate) fn recipe_v1_render_op_id_values(&self) -> [NodeId; 12] {
        self.recipe_v1_render_op_ids()
            .map(|(_, render_op_id)| render_op_id)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct GradeNodeDraft {
    pub(crate) recipe_v1_identity: GradeNodeRecipeV1Identity,
    pub(crate) shared: Option<SharedGradeNodeReference>,
    /// Spatial placement is deliberately instance-local. Publishing a Grade
    /// Node shares its adjustment graph, never this photo's mask placement.
    pub(crate) local_mask: Option<MaskDefinition>,
    pub(crate) label: String,
    pub(crate) basic: BasicEditParameters,
    pub(crate) fine: FineEditParameters,
    pub(crate) enabled: bool,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) struct SharedGradeNodeReference {
    pub(crate) layer_id: LayerId,
    pub(crate) revision_id: LayerRevisionId,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub(crate) struct FineEditParameters {
    pub(crate) selective_tone: SelectiveToneParameters,
    pub(crate) perceptual_color: PerceptualColorParameters,
    pub(crate) oklab_color_warper: OklabColorWarperParameters,
    pub(crate) oklab_lightness_curve: Option<OklabLightnessToneCurve>,
    pub(crate) lut: LutEditParameters,
    pub(crate) sharpen: SharpenParameters,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct LutEditParameters {
    pub(crate) resource_id: String,
    pub(crate) title: String,
    pub(crate) managed_path: String,
    pub(crate) intensity: f64,
}

impl Default for LutEditParameters {
    fn default() -> Self {
        Self {
            resource_id: String::new(),
            title: String::new(),
            managed_path: String::new(),
            intensity: 1.0,
        }
    }
}

const LOCAL_MASK_NONE: u8 = 0;
const LOCAL_MASK_LINEAR_GRADIENT: u8 = 1;
const LOCAL_MASK_RADIAL_GRADIENT: u8 = 2;
const LOCAL_MASK_BRUSH: u8 = 3;

type FfiLocalMaskFields = (u8, f64, f64, f64, f64, f64, f64, f64, bool, Vec<f64>);

fn ffi_local_mask_fields(mask: Option<&MaskDefinition>) -> FfiLocalMaskFields {
    match mask {
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
    }
}

fn local_mask_definition_from_ffi(
    grade_node: &ffi::FfiGradeNode,
    index: usize,
) -> AnyResult<Option<MaskDefinition>> {
    let unit = |name: &str, value: f64| {
        UnitInterval::new(value)
            .with_context(|| format!("Grade Node {index} local mask {name} must be in [0, 1]"))
    };
    match grade_node.local_mask_kind {
        LOCAL_MASK_NONE => Ok(None),
        LOCAL_MASK_LINEAR_GRADIENT => Ok(Some(MaskDefinition::linear_gradient(
            unit("start x", grade_node.local_mask_x0)?,
            unit("start y", grade_node.local_mask_y0)?,
            unit("end x", grade_node.local_mask_x1)?,
            unit("end y", grade_node.local_mask_y1)?,
            grade_node.local_mask_invert,
        )?)),
        LOCAL_MASK_RADIAL_GRADIENT => Ok(Some(MaskDefinition::radial_gradient(
            unit("center x", grade_node.local_mask_x0)?,
            unit("center y", grade_node.local_mask_y0)?,
            unit("radius x", grade_node.local_mask_radius_x)?,
            unit("radius y", grade_node.local_mask_radius_y)?,
            unit("feather", grade_node.local_mask_feather)?,
            grade_node.local_mask_invert,
        )?)),
        LOCAL_MASK_BRUSH => {
            if grade_node.local_mask_brush_points.len() % 3 != 0 {
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
            Ok(Some(MaskDefinition::brush(
                points,
                unit("brush radius", grade_node.local_mask_radius_x)?,
                unit("brush feather", grade_node.local_mask_feather)?,
                grade_node.local_mask_invert,
            )?))
        }
        other => bail!("Grade Node {index} has unsupported local mask kind {other}"),
    }
}

impl GradeNodeDraft {
    pub(crate) fn neutral(label: impl Into<String>) -> Self {
        Self {
            recipe_v1_identity: GradeNodeRecipeV1Identity::new(),
            shared: None,
            local_mask: None,
            label: label.into(),
            basic: BasicEditParameters::default(),
            fine: FineEditParameters::default(),
            enabled: true,
        }
    }

    #[cfg(test)]
    pub(crate) fn duplicate(&self) -> Self {
        Self {
            recipe_v1_identity: GradeNodeRecipeV1Identity::new(),
            // Duplicating is an explicit independent copy. The new node keeps
            // the rendered controls but must never inherit the source link.
            shared: None,
            local_mask: self.local_mask.clone(),
            label: self.label.clone(),
            basic: self.basic,
            fine: self.fine.clone(),
            enabled: self.enabled,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct GradeStackDraft {
    pub(crate) optics: RecipeOpticsSettings,
    pub(crate) grade_nodes: Vec<GradeNodeDraft>,
    /// Photo-local small repairs run after all Grade Nodes. They deliberately
    /// remain outside a reusable Grade Node graph.
    pub(crate) retouch_spots: Vec<RetouchSpot>,
    /// Photo-local continuous repair/clone brush strokes. These remain
    /// separate from legacy circular spots so one drag is one durable edit.
    pub(crate) retouch_strokes: Vec<RetouchStroke>,
    /// Final-canvas crop and orientation. This is photo-local for the same
    /// reason retouch is: a reusable Grade Node cannot decide another photo's
    /// framing.
    pub(crate) geometry: PhotoGeometry,
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

fn ffi_photo_geometry(geometry: PhotoGeometry) -> ffi::FfiPhotoGeometry {
    ffi::FfiPhotoGeometry {
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

fn adjustment_geometry(geometry: PhotoGeometry) -> AdjustmentGeometry {
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

pub(crate) fn bridge_optics_settings(settings: &ffi::FfiOpticsSettings) -> OpticsSettings {
    OpticsSettings {
        enabled: settings.enabled,
        correct_distortion: settings.correct_distortion,
        correct_tca: settings.correct_tca,
        correct_vignetting: settings.correct_vignetting,
        automatic_scale: settings.automatic_scale,
        manual_distortion: settings.manual_distortion,
        manual_tca_red_cyan: settings.manual_tca_red_cyan,
        manual_tca_blue_yellow: settings.manual_tca_blue_yellow,
        manual_vignetting_amount: settings.manual_vignetting_amount,
        manual_vignetting_midpoint: settings.manual_vignetting_midpoint,
        camera_profile_maker: settings.camera_profile_maker.clone(),
        camera_profile_model: settings.camera_profile_model.clone(),
        lens_profile_maker: settings.lens_profile_maker.clone(),
        lens_profile_model: settings.lens_profile_model.clone(),
    }
}

impl Default for GradeStackDraft {
    fn default() -> Self {
        Self {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![GradeNodeDraft::neutral(BASIC_LAYER_LABEL)],
            retouch_spots: Vec::new(),
            retouch_strokes: Vec::new(),
            geometry: PhotoGeometry::identity(),
        }
    }
}

impl std::ops::Deref for GradeStackDraft {
    type Target = GradeNodeDraft;

    fn deref(&self) -> &Self::Target {
        self.grade_nodes
            .first()
            .expect("validated Grade Stack always contains one Grade Node")
    }
}

impl std::ops::DerefMut for GradeStackDraft {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.grade_nodes
            .first_mut()
            .expect("validated Grade Stack always contains one Grade Node")
    }
}

pub(crate) fn new_basic_grade_node(label: &str) -> AnyResult<ffi::FfiGradeNode> {
    let grade_node = GradeNodeDraft::neutral(label);
    let grade_stack = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![grade_node.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };
    grade_stack_recipe_v1_snapshot(&grade_stack, None).context("validate new Basic Grade Node")?;
    Ok(encode_grade_node_draft_recipe_v1(grade_node))
}

pub(crate) fn decode_grade_stack_draft_recipe_v1(
    settings: &ffi::FfiEditSettings,
) -> AnyResult<GradeStackDraft> {
    if !(1..=MAX_GRADE_NODES).contains(&settings.grade_nodes.len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    let grade_stack = GradeStackDraft {
        optics: recipe_optics_settings(&settings.optics),
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
                    .with_context(|| format!("retouch stroke {index} is invalid"))
            })
            .collect::<AnyResult<Vec<_>>>()?,
        geometry: photo_geometry_from_ffi(&settings.geometry)?,
    };
    validate_grade_stack_draft_recipe_v1(&grade_stack)?;
    // Domain construction authoritatively validates labels and the complete
    // graph generated from the untrusted desktop DTO.
    grade_stack_recipe_v1_snapshot(&grade_stack, None).context("validate Grade Stack Recipe v1")?;
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
        local_mask: local_mask_definition_from_ffi(grade_node, index)?,
        label: grade_node.label.clone(),
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

pub(crate) fn validate_grade_stack_draft_recipe_v1(grade_stack: &GradeStackDraft) -> AnyResult<()> {
    if !(1..=MAX_GRADE_NODES).contains(&grade_stack.grade_nodes.len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    if grade_stack.retouch_spots.len() > MAX_RETOUCH_SPOTS_PER_RECIPE {
        bail!(
            "Grade Stack contains {} repair spots, but at most {} are supported",
            grade_stack.retouch_spots.len(),
            MAX_RETOUCH_SPOTS_PER_RECIPE
        );
    }
    for spot in &grade_stack.retouch_spots {
        RetouchSpot::new(
            spot.center_x(),
            spot.center_y(),
            spot.radius_level_zero_pixels(),
        )?
        .with_behavior(
            spot.mode(),
            spot.source_offset_x_radii(),
            spot.source_offset_y_radii(),
            spot.feather(),
        )?;
    }
    if grade_stack.retouch_strokes.len() > MAX_RETOUCH_STROKES_PER_RECIPE {
        bail!(
            "Grade Stack contains {} repair strokes, but at most {} are supported",
            grade_stack.retouch_strokes.len(),
            MAX_RETOUCH_STROKES_PER_RECIPE
        );
    }
    for stroke in &grade_stack.retouch_strokes {
        RetouchStroke::new(stroke.points().to_vec(), stroke.radius_level_zero_pixels())?
            .with_behavior(
                stroke.mode(),
                stroke.source_offset_x_radii(),
                stroke.source_offset_y_radii(),
                stroke.feather(),
            )?;
    }
    let mut grade_node_ids = HashSet::with_capacity(grade_stack.grade_nodes.len());
    let mut render_op_ids = HashSet::with_capacity(grade_stack.grade_nodes.len() * 11);
    for (index, grade_node) in grade_stack.grade_nodes.iter().enumerate() {
        let identity = &grade_node.recipe_v1_identity;
        if !grade_node_ids.insert(identity.grade_node_id) {
            bail!(
                "Grade Stack contains duplicate Grade Node id {}",
                identity.grade_node_id
            );
        }
        for (role, render_op_id) in identity.recipe_v1_render_op_ids() {
            if !render_op_ids.insert(render_op_id) {
                bail!(
                    "Grade Stack contains duplicate Recipe v1 render-op id {render_op_id} at Grade Node {index} role {role}"
                );
            }
        }
        validate_basic_parameters(grade_node.basic)?;
        validate_fine_parameters(&grade_node.fine)?;
    }
    Ok(())
}

pub(crate) fn validate_grade_stack_draft_against_recipe_v1_template(
    grade_stack: &GradeStackDraft,
    template: &RecipeSnapshot,
) -> AnyResult<()> {
    let template_grade_stack = decode_grade_stack_draft_from_recipe_v1_snapshot(template)
        .context("validate base Grade Stack Recipe v1")?;
    let template_grade_nodes = template_grade_stack
        .grade_nodes
        .iter()
        .map(|grade_node| (grade_node.recipe_v1_identity.grade_node_id, grade_node))
        .collect::<HashMap<_, _>>();
    let template_render_ops = template_grade_stack
        .grade_nodes
        .iter()
        .flat_map(|grade_node| {
            let grade_node_id = grade_node.recipe_v1_identity.grade_node_id;
            grade_node
                .recipe_v1_identity
                .recipe_v1_render_op_ids()
                .map(move |(role, render_op_id)| (render_op_id, (grade_node_id, role)))
        })
        .collect::<HashMap<_, _>>();

    for grade_node in &grade_stack.grade_nodes {
        let identity = &grade_node.recipe_v1_identity;
        if let Some(template_grade_node) = template_grade_nodes.get(&identity.grade_node_id)
            && identity != &template_grade_node.recipe_v1_identity
        {
            bail!(
                "retained Grade Node {} must preserve every stable Recipe v1 render-op identity from its base Recipe",
                identity.grade_node_id
            );
        }
        for (role, render_op_id) in identity.recipe_v1_render_op_ids() {
            if let Some((template_grade_node_id, template_role)) =
                template_render_ops.get(&render_op_id)
                && (*template_grade_node_id != identity.grade_node_id || *template_role != role)
            {
                bail!(
                    "Grade Node {} role {role} reuses base Recipe v1 render-op id {render_op_id} owned by Grade Node {template_grade_node_id} role {template_role}",
                    identity.grade_node_id
                );
            }
        }
    }
    Ok(())
}

#[allow(clippy::float_cmp)] // The persisted contract requires exact normalized x endpoints.
pub(crate) fn validate_tone_curve(points: &[ToneCurvePoint]) -> AnyResult<()> {
    if !(2..=MAX_TONE_CURVE_POINTS).contains(&points.len()) {
        bail!("Tone Curve must contain 2 through 256 points");
    }
    if points
        .iter()
        .any(|point| !point.x.is_finite() || !point.y.is_finite())
    {
        bail!("Tone Curve points must contain only finite values");
    }
    if points.first().is_none_or(|point| point.x != 0.0)
        || points.last().is_none_or(|point| point.x != 1.0)
    {
        bail!("Tone Curve x coordinates must start at zero and end at one");
    }
    for pair in points.windows(2) {
        let [left, right] = pair else {
            unreachable!("windows(2) always returns two points")
        };
        if right.x <= left.x {
            bail!("Tone Curve x coordinates must be strictly increasing");
        }
        if !((right.y - left.y) / (right.x - left.x)).is_finite() {
            bail!("Tone Curve segment slopes must be finite");
        }
    }
    Ok(())
}

pub(crate) fn validate_basic_parameters(parameters: BasicEditParameters) -> AnyResult<()> {
    validate_range(parameters.exposure_stops, -16.0, 16.0, "exposure stops")?;
    validate_range(parameters.contrast_factor, 0.0, 8.0, "contrast factor")?;
    validate_range(
        parameters.white_balance_temperature,
        -1.0,
        1.0,
        "RGB white balance temperature",
    )?;
    validate_range(
        parameters.white_balance_tint,
        -1.0,
        1.0,
        "RGB white balance tint",
    )?;
    validate_range(parameters.saturation_factor, 0.0, 8.0, "saturation factor")
}

pub(crate) fn validate_lut_parameters(parameters: &LutEditParameters) -> AnyResult<()> {
    validate_range(parameters.intensity, 0.0, 1.0, "LUT intensity")?;
    if parameters.resource_id.is_empty() {
        if !parameters.title.is_empty() || !parameters.managed_path.is_empty() {
            bail!("an unselected LUT must not retain title or managed path");
        }
        return Ok(());
    }
    if parameters.resource_id.len() != 64
        || !parameters
            .resource_id
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        bail!("LUT resource id must be a lowercase SHA-256 digest");
    }
    if parameters.title.trim().is_empty() || parameters.title.len() > 512 {
        bail!("selected LUT title must contain 1 through 512 bytes");
    }
    let managed_path = Path::new(&parameters.managed_path);
    if !managed_path.is_absolute()
        || managed_path.extension().and_then(|value| value.to_str()) != Some("cube")
        || managed_path.file_stem().and_then(|value| value.to_str())
            != Some(parameters.resource_id.as_str())
    {
        bail!("selected LUT must reference its content-addressed managed .cube path");
    }
    Ok(())
}

pub(crate) fn validate_fine_parameters(parameters: &FineEditParameters) -> AnyResult<()> {
    let tone = parameters.selective_tone;
    for (name, value) in [
        ("highlights", tone.highlights),
        ("shadows", tone.shadows),
        ("whites", tone.whites),
        ("blacks", tone.blacks),
    ] {
        validate_range(value, -1.0, 1.0, name)?;
    }
    let color = &parameters.perceptual_color;
    validate_range(color.global_a_balance, -1.0, 1.0, "global Oklab a balance")?;
    validate_range(color.global_b_balance, -1.0, 1.0, "global Oklab b balance")?;
    validate_range(color.vibrance, -1.0, 1.0, "vibrance")?;
    for (name, values) in [
        ("Color Mixer hue", color.hue_shifts),
        ("Color Mixer saturation", color.saturation),
        ("Color Mixer lightness", color.lightness),
    ] {
        for value in values {
            validate_range(value, -1.0, 1.0, name)?;
        }
    }
    if 1 + color.additional_color_ranges.len() > MAX_POINT_COLOR_RANGES {
        bail!("Point Color supports at most {MAX_POINT_COLOR_RANGES} ordered ranges");
    }
    for range in std::iter::once(&color.color_range).chain(color.additional_color_ranges.iter()) {
        validate_range(range.center_hue_degrees, 0.0, 360.0, "color range center")?;
        validate_range(range.width_degrees, 1.0, 180.0, "color range width")?;
        validate_range(range.softness, 0.0, 1.0, "color range softness")?;
        validate_range(range.hue_shift_degrees, -180.0, 180.0, "color range hue")?;
        validate_range(range.saturation, -1.0, 1.0, "color range saturation")?;
        validate_range(range.lightness, -1.0, 1.0, "color range lightness")?;
    }
    for value in color.selective_color_cmyk {
        validate_range(value, -1.0, 1.0, "Selective Color CMYK")?;
    }
    let color_warper = &parameters.oklab_color_warper;
    validate_range(
        color_warper.strength,
        0.0,
        1.0,
        "Oklab Color Warper strength",
    )?;
    for point in color_warper.control_points {
        validate_range(
            point.a_offset,
            -OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            "Oklab Color Warper a offset",
        )?;
        validate_range(
            point.b_offset,
            -OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            "Oklab Color Warper b offset",
        )?;
    }
    if let Some(curve) = &parameters.oklab_lightness_curve {
        validate_tone_curve(&curve.lightness).context("validate Oklab lightness curve")?;
    }
    validate_lut_parameters(&parameters.lut)?;
    let sharpen = parameters.sharpen;
    validate_range(sharpen.amount, 0.0, 2.0, "sharpen amount")?;
    validate_range(sharpen.radius, 0.1, 5.0, "sharpen radius")?;
    validate_range(sharpen.threshold, 0.0, 1.0, "sharpen threshold")?;
    validate_range(sharpen.masking, 0.0, 1.0, "sharpen masking")?;
    validate_range(sharpen.local_contrast, -1.0, 1.0, "local contrast")?;
    validate_range(
        sharpen.local_contrast_scale,
        0.0,
        1.0,
        "local contrast scale",
    )?;
    for (name, value) in [
        ("luminance noise reduction", sharpen.denoise_luminance),
        ("noise reduction detail", sharpen.denoise_detail),
        ("color noise reduction", sharpen.denoise_color),
        ("purple defringe amount", sharpen.defringe_purple_amount),
        ("green defringe amount", sharpen.defringe_green_amount),
        ("shadow grading saturation", sharpen.shadows_saturation),
        ("midtone grading saturation", sharpen.midtones_saturation),
        (
            "highlight grading saturation",
            sharpen.highlights_saturation,
        ),
        ("grading blending", sharpen.grading_blending),
        ("grain amount", sharpen.grain_amount),
        ("grain size", sharpen.grain_size),
        ("grain roughness", sharpen.grain_roughness),
        ("vignette midpoint", sharpen.vignette_midpoint),
        ("vignette feather", sharpen.vignette_feather),
        ("vignette highlights", sharpen.vignette_highlights),
    ] {
        validate_range(value, 0.0, 1.0, name)?;
    }
    for (name, value) in [
        ("dehaze", sharpen.dehaze),
        ("shadow grading luminance", sharpen.shadows_luminance),
        ("midtone grading luminance", sharpen.midtones_luminance),
        ("highlight grading luminance", sharpen.highlights_luminance),
        ("grading balance", sharpen.grading_balance),
        ("vignette amount", sharpen.vignette_amount),
        ("vignette roundness", sharpen.vignette_roundness),
    ] {
        validate_range(value, -1.0, 1.0, name)?;
    }
    for (name, value) in [
        ("shadow grading hue", sharpen.shadows_hue),
        ("midtone grading hue", sharpen.midtones_hue),
        ("highlight grading hue", sharpen.highlights_hue),
    ] {
        validate_range(value, 0.0, 360.0, name)?;
    }
    for (name, value) in [
        ("purple defringe hue low", sharpen.defringe_purple_hue_low),
        ("purple defringe hue high", sharpen.defringe_purple_hue_high),
        ("green defringe hue low", sharpen.defringe_green_hue_low),
        ("green defringe hue high", sharpen.defringe_green_hue_high),
    ] {
        validate_range(value, 0.0, 360.0, name)?;
    }
    if sharpen.defringe_purple_hue_low + 10.0 > sharpen.defringe_purple_hue_high
        || sharpen.defringe_green_hue_low + 10.0 > sharpen.defringe_green_hue_high
    {
        bail!("defringe hue ranges must have at least a 10 degree span");
    }
    Ok(())
}

pub(crate) fn validate_range(value: f64, minimum: f64, maximum: f64, name: &str) -> AnyResult<()> {
    if value.is_finite() && (minimum..=maximum).contains(&value) {
        Ok(())
    } else {
        bail!("{name} must be finite and in {minimum}..={maximum}")
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
) -> ffi::FfiEditSettings {
    ffi::FfiEditSettings {
        optics: ffi_optics_settings(&grade_stack.optics),
        grade_nodes: grade_stack
            .grade_nodes
            .into_iter()
            .map(encode_grade_node_draft_recipe_v1)
            .collect(),
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
            })
            .collect(),
        geometry: ffi_photo_geometry(grade_stack.geometry),
    }
}

pub(crate) fn encode_grade_node_draft_recipe_v1(grade_node: GradeNodeDraft) -> ffi::FfiGradeNode {
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
    ) = ffi_local_mask_fields(grade_node.local_mask.as_ref());
    ffi::FfiGradeNode {
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
    }
}

//! Recipe v1's complete desktop adapter contract.
//!
//! This module owns the editable Grade Stack draft, validation, immutable
//! RecipeSnapshot serialization, reverse decoding, and compilation into the
//! typed image render plan. The desktop session remains only the orchestration
//! facade around this contract.

use super::*;

const _: () = assert!(
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION == ADJUSTMENT_PARAMETER_SCHEMA_VERSION
        && CPU_REFERENCE_IMPLEMENTATION_REVISION == ADJUSTMENT_IMPLEMENTATION_VERSION
        && OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION
            == OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_REVISION
        && SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION
            == SELECTIVE_TONE_V3_PARAMETER_SCHEMA_REVISION
);

pub(crate) const MAX_GRADE_NODES: usize = 16;
pub(crate) const RECIPE_V1_OKLAB_LIGHTNESS_TONE_CURVE_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.oklab-lightness-tone-curve-slot-id.v1\0";
pub(crate) const RECIPE_V1_SELECTIVE_TONE_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.selective-tone-slot-id.v1\0";
pub(crate) const RECIPE_V1_PERCEPTUAL_COLOR_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.perceptual-color-slot-id.v1\0";
pub(crate) const RECIPE_V1_LUT_RENDER_OP_ID_DOMAIN: &[u8] = b"shadow.desktop.lut-slot-id.v1\0";
/// A local spatial mask belongs to the *instance* of a Grade Node. Its
/// generated identity incorporates the shape payload, so two immutable recipe
/// snapshots can never claim that one mask revision means different pixels.
pub(crate) const RECIPE_V1_LOCAL_MASK_ID_DOMAIN: &[u8] =
    b"shadow.desktop.local-mask-revision-id.v1\0";
// Retouch is photo-local rather than a Grade Node. These fixed, compiler-only
// stream identities keep it deterministic without making it user-visible or
// shareable by mistake.
const RECIPE_V1_RETOUCH_LAYER_START_ID: &str = "recipe-v1-photo-retouch:start";
const RECIPE_V1_RETOUCH_RENDER_NODE_ID: &str = "recipe-v1-photo-retouch:spots";
const RECIPE_V1_RETOUCH_LAYER_END_ID: &str = "recipe-v1-photo-retouch:end";
// The external Qt DTO keeps its historical `sharpen_render_op_id` slot, but
// schema 3 gives it the technical-detail role. The two new internal slots are
// deterministic from the Grade Node identity and intentionally never leak as
// extra UI controls.
pub(crate) const RECIPE_V3_TECHNICAL_DETAIL_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.technical-detail-slot-id.v3\0";
pub(crate) const RECIPE_V3_COLOR_GRADING_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.color-grading-slot-id.v3\0";
pub(crate) const RECIPE_V3_FINISHING_EFFECTS_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.finishing-effects-slot-id.v3\0";

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

pub(crate) fn recipe_v1_sharpen_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V3_TECHNICAL_DETAIL_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

pub(crate) fn recipe_v3_color_grading_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V3_COLOR_GRADING_RENDER_OP_ID_DOMAIN, grade_node_id)
}

pub(crate) fn recipe_v3_finishing_effects_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V3_FINISHING_EFFECTS_RENDER_OP_ID_DOMAIN,
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
            lut_render_op_id: recipe_v1_lut_render_op_id(grade_node_id),
            sharpen_render_op_id: recipe_v1_sharpen_render_op_id(grade_node_id),
            color_grading_render_op_id: recipe_v3_color_grading_render_op_id(grade_node_id),
            finishing_effects_render_op_id: recipe_v3_finishing_effects_render_op_id(grade_node_id),
        }
    }

    /// Recipe v1 stores the controls inside one Grade Node as atomic
    /// `AdjustmentNode`s. These are compiler/adapter identities, not Grade
    /// Nodes exposed to the product surface.
    fn recipe_v1_render_op_ids(&self) -> [(&'static str, NodeId); 11] {
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
            ("color_grading", self.color_grading_render_op_id),
            ("lut", self.lut_render_op_id),
            ("technical_detail", self.sharpen_render_op_id),
            ("finishing_effects", self.finishing_effects_render_op_id),
        ]
    }

    #[cfg(test)]
    pub(crate) fn recipe_v1_render_op_id_values(&self) -> [NodeId; 11] {
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

type FfiLocalMaskFields = (u8, f64, f64, f64, f64, f64, f64, f64, bool);

fn ffi_local_mask_fields(mask: Option<&MaskDefinition>) -> FfiLocalMaskFields {
    match mask {
        None => (LOCAL_MASK_NONE, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false),
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
                RetouchSpot::new(
                    UnitInterval::new(spot.center_x).with_context(|| {
                        format!("retouch spot {index} center x must be in [0, 1]")
                    })?,
                    UnitInterval::new(spot.center_y).with_context(|| {
                        format!("retouch spot {index} center y must be in [0, 1]")
                    })?,
                    spot.radius_level_zero_pixels,
                )
                .with_context(|| format!("retouch spot {index} is invalid"))
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
            lut_render_op_id: parse_render_op_id("LUT", &grade_node.lut_render_op_id)?,
            sharpen_render_op_id: parse_render_op_id("sharpen", &grade_node.sharpen_render_op_id)?,
            color_grading_render_op_id: recipe_v3_color_grading_render_op_id(grade_node_id),
            finishing_effects_render_op_id: recipe_v3_finishing_effects_render_op_id(grade_node_id),
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
    if let Some(curve) = &parameters.oklab_lightness_curve {
        validate_tone_curve(&curve.lightness).context("validate Oklab lightness curve")?;
    }
    validate_lut_parameters(&parameters.lut)?;
    let sharpen = parameters.sharpen;
    validate_range(sharpen.amount, 0.0, 2.0, "sharpen amount")?;
    validate_range(sharpen.radius, 0.1, 5.0, "sharpen radius")?;
    validate_range(sharpen.threshold, 0.0, 1.0, "sharpen threshold")?;
    validate_range(sharpen.masking, 0.0, 1.0, "sharpen masking")?;
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

/// Compiles the currently executable Recipe v1 adapter subset into dependency
/// order. Recipe `LayerInstance` vector order is the Grade Node execution
/// order; graph bindings and the explicit output node define the private
/// render-operation order within each Grade Node.
pub(crate) fn compile_recipe_render_plan(
    snapshot: &RecipeSnapshot,
) -> AnyResult<AdjustmentRenderPlan> {
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

    // A photo-local repair must run after every Grade Node. It uses an
    // unmasked boundary layer so the native executor can keep one ordering
    // grammar for both local Grade Nodes and photo-local spatial operations.
    let has_retouch = !snapshot.retouch_spots().is_empty();
    let use_layer_boundaries =
        has_retouch || snapshot.layers().iter().any(|layer| layer.mask().is_some());
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
                    Some(adjustment_local_mask(definition.definition()))
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
            enabled: true,
            operation: AdjustmentRenderOperation::SpotHeal {
                targets: snapshot
                    .retouch_spots()
                    .iter()
                    .map(|spot| AdjustmentSpotHealTarget {
                        center_x: spot.center_x().get(),
                        center_y: spot.center_y().get(),
                        radius_level_zero_pixels: spot.radius_level_zero_pixels(),
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
    }
    if compiled.len() > MAX_ADJUSTMENT_RENDER_NODES {
        bail!("Recipe render compiler supports at most 256 executable nodes");
    }
    let plan = AdjustmentRenderPlan {
        nodes: compiled,
        geometry: adjustment_geometry(snapshot.geometry()),
    };
    plan.validate()
        .context("validate compiled Recipe render plan")?;
    Ok(plan)
}

fn adjustment_local_mask(definition: &MaskDefinition) -> AdjustmentLocalMask {
    match definition {
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
    }
}

pub(crate) fn ordered_layer_nodes(layer: &LayerInstance) -> AnyResult<Vec<&AdjustmentNode>> {
    if layer.scope() != AdjustmentScope::Photo
        || layer.opacity() != UnitInterval::ONE
        || layer.blend_mode() != BlendMode::Normal
    {
        bail!("Recipe render compiler does not support this layer scope, blend, or opacity");
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
        && descriptor.parameter_schema_version() == SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION;
    let is_current_perceptual_color = descriptor.operation_id().as_str()
        == PERCEPTUAL_COLOR_OPERATION_ID
        && descriptor.parameter_schema_version() == PERCEPTUAL_COLOR_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == PERCEPTUAL_COLOR_V3_IMPLEMENTATION_VERSION;
    let is_current_technical_detail = descriptor.operation_id().as_str()
        == TECHNICAL_DETAIL_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION;
    let is_current_color_grading = descriptor.operation_id().as_str() == COLOR_GRADING_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == COLOR_GRADING_V3_IMPLEMENTATION_VERSION;
    let is_current_finishing_effects = descriptor.operation_id().as_str()
        == FINISHING_EFFECTS_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION;
    if (!is_base_contract
        && !is_current_oklab_lightness_tone_curve
        && !is_current_selective_tone
        && !is_current_perceptual_color
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
                parameters: SelectiveToneParameters {
                    highlights: required_float(node.parameters(), HIGHLIGHTS_PARAMETER_KEY, 4)?,
                    shadows: required_float(node.parameters(), SHADOWS_PARAMETER_KEY, 4)?,
                    whites: required_float(node.parameters(), WHITES_PARAMETER_KEY, 4)?,
                    blacks: required_float(node.parameters(), BLACKS_PARAMETER_KEY, 4)?,
                },
            }
        }
        PERCEPTUAL_COLOR_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_perceptual_color {
                bail!("Recipe Color Mixer uses a discarded contract");
            }
            let expected_len = 15;
            AdjustmentRenderOperation::PerceptualColor {
                parameters: Box::new(PerceptualColorParameters {
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
            AdjustmentRenderOperation::Sharpen {
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
            SELECTIVE_TONE_V3_IMPLEMENTATION_REVISION
        } else if is_current_perceptual_color {
            PERCEPTUAL_COLOR_V3_IMPLEMENTATION_REVISION
        } else if is_current_oklab_lightness_tone_curve {
            OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_REVISION
        } else if is_current_technical_detail {
            TECHNICAL_DETAIL_V3_IMPLEMENTATION_REVISION
        } else if is_current_color_grading {
            COLOR_GRADING_V3_IMPLEMENTATION_REVISION
        } else if is_current_finishing_effects {
            FINISHING_EFFECTS_V3_IMPLEMENTATION_REVISION
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
    RecipeSnapshot::new_with_input_settings_masks_retouch_and_geometry(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::new(grade_stack.optics.clone()),
        recipe_v1_masks,
        grade_stack.retouch_spots.clone(),
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
    // Perceptual L belongs after OKLCH/Selective Color controls (so hue-keyed
    // corrections use their authored source hue), before technical recovery
    // and creative LUTs.
    let perceptual_tone_input = if let Some(curve) = fine.oklab_lightness_curve.as_ref() {
        nodes.push(recipe_oklab_lightness_tone_curve_render_op(
            oklab_lightness_curve_id,
            NodeInput::Node {
                node_id: perceptual_color_id,
            },
            curve,
        )?);
        oklab_lightness_curve_id
    } else {
        perceptual_color_id
    };
    // The former monolithic Detail & Effects node is deliberately expanded
    // here, not in the UI: foundational color and the user curve run before
    // technical recovery; color wheels stay in CreativeColor, and physical
    // finishing is last.
    nodes.push(recipe_detail_effects_render_op(
        technical_detail_id,
        TECHNICAL_DETAIL_OPERATION_ID,
        TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION,
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
            COLOR_GRADING_V3_IMPLEMENTATION_VERSION,
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
            FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION,
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
        SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION,
        SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION,
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
        PERCEPTUAL_COLOR_V3_PARAMETER_SCHEMA_VERSION,
        PERCEPTUAL_COLOR_V3_IMPLEMENTATION_VERSION,
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

pub(crate) fn detail_effect_values(parameters: &SharpenParameters) -> [f64; 31] {
    [
        parameters.clarity,
        parameters.texture,
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
        bail!("Detail & Effects storage must contain exactly 31 values");
    };
    parameters.clarity = *clarity;
    parameters.texture = *texture;
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
        TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION,
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
    pub(crate) technical_detail: &'a AdjustmentNode,
    pub(crate) color_grading: &'a AdjustmentNode,
    pub(crate) lut: &'a AdjustmentNode,
    pub(crate) finishing_effects: &'a AdjustmentNode,
}

impl GradeNodeRecipeV1RenderOps<'_> {
    fn ordered(&self) -> Vec<&AdjustmentNode> {
        let mut nodes = vec![
            self.white_balance,
            self.exposure,
            self.contrast,
            self.selective_tone,
            self.saturation,
            self.perceptual_color,
        ];
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
    if !(10..=11).contains(&ordered.len()) {
        bail!("working Recipe is not the current complete Grade Node shape");
    }
    let white_balance = ordered[0];
    let exposure = ordered[1];
    let contrast = ordered[2];
    let selective_tone = ordered[3];
    let saturation = ordered[4];
    let perceptual_color = ordered[5];
    let mut cursor = 6_usize;
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
        TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION,
        ProcessingStage::TechnicalDetail,
        NodeInput::Node {
            node_id: technical_input,
        },
    )?;
    validate_recipe_detail_effects_render_op(
        color_grading,
        COLOR_GRADING_OPERATION_ID,
        COLOR_GRADING_V3_IMPLEMENTATION_VERSION,
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
        FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION,
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
        technical_detail,
        color_grading,
        lut,
        finishing_effects,
    })
}

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
    let node = nodes.selective_tone;
    let selective_tone = SelectiveToneParameters {
        highlights: required_float(node.parameters(), HIGHLIGHTS_PARAMETER_KEY, 4)?,
        shadows: required_float(node.parameters(), SHADOWS_PARAMETER_KEY, 4)?,
        whites: required_float(node.parameters(), WHITES_PARAMETER_KEY, 4)?,
        blacks: required_float(node.parameters(), BLACKS_PARAMETER_KEY, 4)?,
    };
    let perceptual_color = {
        let node = nodes.perceptual_color;
        let expected_len = 15;
        PerceptualColorParameters {
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
        // The three schema-3 passes intentionally carry the same visible
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
        oklab_lightness_curve,
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
        optics: snapshot.input_settings().optics().clone(),
        grade_nodes: snapshot
            .layers()
            .iter()
            .map(|layer| {
                let local_mask = recipe_v1_local_mask_from_snapshot(snapshot, layer)?;
                decode_grade_node_draft_from_recipe_v1_layer(layer, local_mask)
            })
            .collect::<AnyResult<Vec<_>>>()?,
        retouch_spots: snapshot.retouch_spots().to_vec(),
        geometry: snapshot.geometry(),
    };
    validate_grade_stack_draft_recipe_v1(&grade_stack)?;
    Ok(grade_stack)
}

pub(crate) fn decode_grade_node_draft_from_recipe_v1_layer(
    layer: &LayerInstance,
    local_mask: Option<MaskDefinition>,
) -> AnyResult<GradeNodeDraft> {
    let nodes = grade_node_recipe_v1_render_ops(layer)?;
    if nodes.color_grading.id() != recipe_v3_color_grading_render_op_id(layer.id())
        || nodes.finishing_effects.id() != recipe_v3_finishing_effects_render_op_id(layer.id())
        || nodes.oklab_lightness_curve.is_some_and(|node| {
            node.id() != recipe_v1_oklab_lightness_tone_curve_render_op_id(layer.id())
        })
    {
        bail!("working Recipe uses non-canonical internal Detail & Effects pass identities");
    }
    let basic = basic_parameters_from_nodes(&nodes)?;
    let fine = fine_parameters_from_nodes(&nodes)?;
    Ok(GradeNodeDraft {
        recipe_v1_identity: GradeNodeRecipeV1Identity {
            grade_node_id: layer.id(),
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
        local_mask,
        basic,
        fine,
        enabled: layer.enabled(),
    })
}

fn recipe_v1_local_mask_from_snapshot(
    snapshot: &RecipeSnapshot,
    layer: &LayerInstance,
) -> AnyResult<Option<MaskDefinition>> {
    let Some(reference) = layer.mask() else {
        return Ok(None);
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
    Ok(Some(mask.definition().clone()))
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
    decode_grade_node_draft_from_recipe_v1_layer(&layer, None)
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
        )?),
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
        == SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION;
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
        == PERCEPTUAL_COLOR_V3_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == PERCEPTUAL_COLOR_V3_IMPLEMENTATION_VERSION;
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
        == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
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

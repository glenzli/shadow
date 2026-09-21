//! Deterministic identities reserved by the Recipe v1 desktop adapter.

use anyhow::{Context, Result as AnyResult};
use shadow_domain::{
    EntityId, LayerInstanceId, MaskCoordinateSpace, MaskDefinition, MaskId, MaskRevision, NodeId,
};
use uuid::Uuid;

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

pub(crate) fn recipe_v1_rgb_tone_curves_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        b"shadow.desktop.rgb-tone-curves-slot-id.v1\0",
        grade_node_id,
    )
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

pub(crate) fn recipe_v1_local_mask_revision(
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
    pub(crate) rgb_tone_curves_render_op_id: NodeId,
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
            rgb_tone_curves_render_op_id: recipe_v1_rgb_tone_curves_render_op_id(grade_node_id),
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
    pub(crate) fn recipe_v1_render_op_ids(&self) -> [(&'static str, NodeId); 13] {
        [
            ("rgb_tone_curves", self.rgb_tone_curves_render_op_id),
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
    pub(crate) fn recipe_v1_render_op_id_values(&self) -> [NodeId; 13] {
        self.recipe_v1_render_op_ids()
            .map(|(_, render_op_id)| render_op_id)
    }
}

#[cfg(test)]
mod tests;

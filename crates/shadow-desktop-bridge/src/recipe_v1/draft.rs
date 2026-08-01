//! Editable Grade Stack data model used by every Recipe v1 adapter boundary.

use shadow_bridge::{
    BasicEditParameters, OklabColorWarperParameters, OklabLightnessToneCurve,
    PerceptualColorParameters, SelectiveToneParameters, SharpenParameters,
};
use shadow_domain::{
    LayerId, LayerRevisionId, MaskDefinition, PhotoCanvasNode, PhotoFoundationNode,
    PhotoLiquifyNode, RawFoundationDenoise, RetouchSpot, RetouchStroke,
    operation::BASIC_LAYER_LABEL,
};

use super::GradeNodeRecipeV1Identity;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) struct PreservedManagedRasterSettings {
    pub(crate) expansion_percent: i8,
    pub(crate) feather_percent: u8,
    pub(crate) invert: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct GradeNodeDraft {
    pub(crate) recipe_v1_identity: GradeNodeRecipeV1Identity,
    pub(crate) shared: Option<SharedGradeNodeReference>,
    /// Spatial placement is deliberately instance-local. Publishing a Grade
    /// Node shares its adjustment graph, never this photo's mask placement.
    pub(crate) local_mask: Option<MaskDefinition>,
    /// The Qt DTO carries persisted managed rasters as an opaque kind-six
    /// marker plus reversible refinement controls. Snapshot encoding must
    /// recover the exact immutable raster reference from the explicit base
    /// Recipe before applying these settings.
    pub(crate) preserved_managed_raster: Option<PreservedManagedRasterSettings>,
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

impl GradeNodeDraft {
    pub(crate) fn neutral(label: impl Into<String>) -> Self {
        Self {
            recipe_v1_identity: GradeNodeRecipeV1Identity::new(),
            shared: None,
            local_mask: None,
            preserved_managed_raster: None,
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
            preserved_managed_raster: self.preserved_managed_raster,
            label: self.label.clone(),
            basic: self.basic,
            fine: self.fine.clone(),
            enabled: self.enabled,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct GradeStackDraft {
    /// Optional fixed, photo-private AI RAW denoise node evaluated before Foundation.
    pub(crate) raw_ai_denoise: RawFoundationDenoise,
    /// Mandatory photo-private source-development state. The desktop optics
    /// controls are currently only one projection of this singleton.
    pub(crate) foundation: PhotoFoundationNode,
    pub(crate) grade_nodes: Vec<GradeNodeDraft>,
    /// Photo-local small repairs run after all Grade Nodes. They deliberately
    /// remain outside a reusable Grade Node graph.
    pub(crate) retouch_spots: Vec<RetouchSpot>,
    /// Photo-local continuous repair/clone brush strokes. These remain
    /// separate from legacy circular spots so one drag is one durable edit.
    pub(crate) retouch_strokes: Vec<RetouchStroke>,
    /// Optional singleton photo-private Liquify node. It is structural,
    /// non-shareable, and always evaluates immediately before Canvas.
    pub(crate) liquify: Option<PhotoLiquifyNode>,
    /// Editable projection of the optional fixed photo-private Canvas node.
    pub(crate) canvas: PhotoCanvasNode,
}

impl Default for GradeStackDraft {
    fn default() -> Self {
        Self {
            raw_ai_denoise: RawFoundationDenoise::default(),
            foundation: PhotoFoundationNode::default(),
            grade_nodes: vec![GradeNodeDraft::neutral(BASIC_LAYER_LABEL)],
            retouch_spots: Vec::new(),
            retouch_strokes: Vec::new(),
            liquify: None,
            canvas: PhotoCanvasNode::identity(),
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

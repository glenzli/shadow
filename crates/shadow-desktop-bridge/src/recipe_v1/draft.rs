//! Editable Grade Stack data model used by every Recipe v1 adapter boundary.

use shadow_bridge::{
    BasicEditParameters, OklabColorWarperParameters, OklabLightnessToneCurve,
    PerceptualColorParameters, SelectiveToneParameters, SharpenParameters,
};
use shadow_domain::{
    LayerId, LayerRevisionId, MaskDefinition, PhotoGeometry, RecipeOpticsSettings, RetouchSpot,
    RetouchStroke, operation::BASIC_LAYER_LABEL,
};

use super::GradeNodeRecipeV1Identity;

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

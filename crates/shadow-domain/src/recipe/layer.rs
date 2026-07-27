//! Adjustment-layer instances, scope/blend policy, and immutable shared revisions.

use serde::{Deserialize, Serialize};

use crate::{
    CollectionId, GroupId, LayerId, LayerInstanceId, LayerRevisionId, OutputTargetId, SelectionId,
    ShootId,
};

use super::value::{MAX_LABEL_BYTES, UnitInterval, display_name_character, validate_text};
use super::{EditGraph, MaskReference, RecipeValidationError};

/// The execution and sharing target of a layer. Photo scope is relative to
/// the recipe owner; broader scopes carry strongly typed persistent IDs.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", content = "target", rename_all = "snake_case")]
pub enum AdjustmentScope {
    Photo,
    Burst(GroupId),
    LightingGroup(GroupId),
    Selection(SelectionId),
    Shoot(ShootId),
    Collection(CollectionId),
    Output(OutputTargetId),
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BlendMode {
    Normal,
    Luminosity,
    Color,
    Multiply,
    Screen,
    SoftLight,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "mode", content = "revision_id", rename_all = "snake_case")]
pub enum LayerRevisionSelector {
    FollowHead,
    Pinned(LayerRevisionId),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum LayerContent {
    Inline {
        graph: EditGraph,
    },
    Shared {
        layer_id: LayerId,
        revision: LayerRevisionSelector,
        /// The exact immutable graph resolved for this recipe instance.
        ///
        /// Shared-library heads may move, but a photo recipe must remain
        /// independently renderable and a committed recipe must reproduce the
        /// pixels from the pinned revision without consulting mutable state.
        graph: EditGraph,
    },
}

impl LayerContent {
    pub const fn follows_head(&self) -> bool {
        matches!(
            self,
            Self::Shared {
                revision: LayerRevisionSelector::FollowHead,
                ..
            }
        )
    }

    /// Returns the executable graph materialized in this recipe.
    pub const fn graph(&self) -> &EditGraph {
        match self {
            Self::Inline { graph } | Self::Shared { graph, .. } => graph,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LayerInstance {
    id: LayerInstanceId,
    label: String,
    scope: AdjustmentScope,
    content: LayerContent,
    enabled: bool,
    opacity: UnitInterval,
    blend_mode: BlendMode,
    mask: Option<MaskReference>,
}

impl LayerInstance {
    /// Creates one ordered use of an inline or shared adjustment layer.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid labels, masks, graphs, opacity, or for an
    /// inline graph claiming a scope broader than the owning photo.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        id: LayerInstanceId,
        label: impl Into<String>,
        scope: AdjustmentScope,
        content: LayerContent,
        enabled: bool,
        opacity: UnitInterval,
        blend_mode: BlendMode,
        mask: Option<MaskReference>,
    ) -> Result<Self, RecipeValidationError> {
        let label = label.into();
        validate_text(
            &label,
            MAX_LABEL_BYTES,
            "layer label",
            display_name_character,
        )?;
        let layer = Self {
            id,
            label,
            scope,
            content,
            enabled,
            opacity,
            blend_mode,
            mask,
        };
        layer.validate()?;
        Ok(layer)
    }

    pub const fn id(&self) -> LayerInstanceId {
        self.id
    }

    pub fn label(&self) -> &str {
        &self.label
    }

    pub const fn scope(&self) -> AdjustmentScope {
        self.scope
    }

    pub fn content(&self) -> &LayerContent {
        &self.content
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    pub const fn opacity(&self) -> UnitInterval {
        self.opacity
    }

    pub const fn blend_mode(&self) -> BlendMode {
        self.blend_mode
    }

    pub const fn mask(&self) -> Option<MaskReference> {
        self.mask
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        validate_text(
            &self.label,
            MAX_LABEL_BYTES,
            "layer label",
            display_name_character,
        )?;
        UnitInterval::new(self.opacity.get())?;
        if self.mask.is_some_and(|mask| mask.revision() == 0) {
            return Err(RecipeValidationError::ZeroMaskRevision);
        }
        self.content.graph().validate()?;
        if let LayerContent::Inline { .. } = &self.content {
            if self.scope != AdjustmentScope::Photo {
                return Err(RecipeValidationError::InlineLayerMustBePhotoScoped {
                    layer_id: self.id,
                });
            }
        }
        Ok(())
    }
}

/// An immutable published definition of a reusable shared adjustment layer.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LayerRevision {
    id: LayerRevisionId,
    layer_id: LayerId,
    revision_number: u32,
    parent: Option<LayerRevisionId>,
    label: String,
    graph: EditGraph,
}

impl LayerRevision {
    /// Publishes an immutable revision of a reusable shared layer.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid labels or graphs, zero revision numbers,
    /// self-parenting, or inconsistent root/parent semantics.
    pub fn new(
        id: LayerRevisionId,
        layer_id: LayerId,
        revision_number: u32,
        parent: Option<LayerRevisionId>,
        label: impl Into<String>,
        graph: EditGraph,
    ) -> Result<Self, RecipeValidationError> {
        let label = label.into();
        validate_text(
            &label,
            MAX_LABEL_BYTES,
            "layer label",
            display_name_character,
        )?;
        if revision_number == 0 {
            return Err(RecipeValidationError::ZeroLayerRevision);
        }
        if parent == Some(id) {
            return Err(RecipeValidationError::SelfParentLayerRevision(id));
        }
        if (revision_number == 1) != parent.is_none() {
            return Err(RecipeValidationError::InvalidLayerRevisionParent {
                revision_number,
                has_parent: parent.is_some(),
            });
        }
        graph.validate()?;
        Ok(Self {
            id,
            layer_id,
            revision_number,
            parent,
            label,
            graph,
        })
    }

    /// Checks a deserialized shared-layer revision.
    ///
    /// # Errors
    ///
    /// Returns the first layer revision invariant violation found.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(
            self.id,
            self.layer_id,
            self.revision_number,
            self.parent,
            self.label.clone(),
            self.graph.clone(),
        )
        .map(|_| ())
    }

    pub const fn id(&self) -> LayerRevisionId {
        self.id
    }

    pub const fn layer_id(&self) -> LayerId {
        self.layer_id
    }

    pub const fn revision_number(&self) -> u32 {
        self.revision_number
    }

    pub const fn parent(&self) -> Option<LayerRevisionId> {
        self.parent
    }

    pub fn label(&self) -> &str {
        &self.label
    }

    pub fn graph(&self) -> &EditGraph {
        &self.graph
    }
}

#[cfg(test)]
mod tests;

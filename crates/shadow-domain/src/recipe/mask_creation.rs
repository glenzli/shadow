//! Explicit node-local mask creation intent.
//!
//! This command vocabulary freezes the destination before an editor mutates a
//! Grade Stack. It deliberately contains no mask name, library identity, or
//! preset link: manual masks are anonymous and a condition preset contributes
//! only a copied typed definition.

use crate::{EntityId, LayerInstanceId};

use super::{ConditionMaskPreset, MaskDefinition, RecipeValidationError};

/// The exact Grade Node destination selected by one mask-creation gesture.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum NodeLocalMaskCreationTarget {
    /// Replace or initialize the local mask on the node that was current when
    /// the command was admitted.
    CurrentNode { node_id: LayerInstanceId },
    /// Insert a fresh neutral node immediately after a frozen anchor and put
    /// the new mask on that node.
    NewNodeAfter {
        anchor_node_id: LayerInstanceId,
        new_node_id: LayerInstanceId,
    },
}

impl NodeLocalMaskCreationTarget {
    pub const fn destination_node_id(self) -> LayerInstanceId {
        match self {
            Self::CurrentNode { node_id } => node_id,
            Self::NewNodeAfter { new_node_id, .. } => new_node_id,
        }
    }
}

/// One admitted anonymous mask creation command.
#[derive(Debug, Clone, PartialEq)]
pub struct NodeLocalMaskCreationIntent {
    target: NodeLocalMaskCreationTarget,
    definition: MaskDefinition,
}

impl NodeLocalMaskCreationIntent {
    /// Creates a command for an explicit destination.
    ///
    /// # Errors
    ///
    /// Returns an error when the owned mask definition is invalid.
    pub fn new(
        target: NodeLocalMaskCreationTarget,
        definition: MaskDefinition,
    ) -> Result<Self, RecipeValidationError> {
        if let NodeLocalMaskCreationTarget::NewNodeAfter {
            anchor_node_id,
            new_node_id,
        } = target
            && anchor_node_id == new_node_id
        {
            return Err(RecipeValidationError::SelfAnchoredMaskDestination(
                new_node_id,
            ));
        }
        definition.validate()?;
        Ok(Self { target, definition })
    }

    /// Targets the node that was current when the command was admitted.
    ///
    /// # Errors
    ///
    /// Returns an error when the mask definition is invalid.
    pub fn for_current_node(
        node_id: LayerInstanceId,
        definition: MaskDefinition,
    ) -> Result<Self, RecipeValidationError> {
        Self::new(
            NodeLocalMaskCreationTarget::CurrentNode { node_id },
            definition,
        )
    }

    /// Reserves a fresh destination node after an explicit anchor.
    ///
    /// The new identity is generated when the command is created, not while it
    /// is applied, so retrying the same admitted command cannot create several
    /// nodes.
    ///
    /// # Errors
    ///
    /// Returns an error when the mask definition is invalid.
    pub fn for_new_node_after(
        anchor_node_id: LayerInstanceId,
        definition: MaskDefinition,
    ) -> Result<Self, RecipeValidationError> {
        Self::new(
            NodeLocalMaskCreationTarget::NewNodeAfter {
                anchor_node_id,
                new_node_id: LayerInstanceId::new_v7(),
            },
            definition,
        )
    }

    /// Copies a preset's typed parameters into the current node.
    ///
    /// # Errors
    ///
    /// Returns an error when the copied expression cannot form a canonical
    /// Recipe-local mask definition.
    pub fn from_preset_for_current_node(
        node_id: LayerInstanceId,
        preset: &ConditionMaskPreset,
    ) -> Result<Self, RecipeValidationError> {
        Self::for_current_node(
            node_id,
            MaskDefinition::condition_expression(preset.copy_expression())?,
        )
    }

    /// Copies a preset's typed parameters into a fresh destination node.
    ///
    /// # Errors
    ///
    /// Returns an error when the copied expression cannot form a canonical
    /// Recipe-local mask definition.
    pub fn from_preset_for_new_node_after(
        anchor_node_id: LayerInstanceId,
        preset: &ConditionMaskPreset,
    ) -> Result<Self, RecipeValidationError> {
        Self::for_new_node_after(
            anchor_node_id,
            MaskDefinition::condition_expression(preset.copy_expression())?,
        )
    }

    pub const fn target(&self) -> NodeLocalMaskCreationTarget {
        self.target
    }

    pub const fn definition(&self) -> &MaskDefinition {
        &self.definition
    }

    pub fn into_parts(self) -> (NodeLocalMaskCreationTarget, MaskDefinition) {
        (self.target, self.definition)
    }
}

#[cfg(test)]
mod tests;

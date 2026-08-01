//! Fixed-order, photo-local structural node topology.

use serde::{Deserialize, Serialize};

use super::{PhotoCanvasNode, PhotoLiquifyNode, RecipeValidationError};

/// A borrowed structural node in its only legal execution order.
#[derive(Debug, Copy, Clone, PartialEq)]
pub enum PhotoStructuralNodeRef<'a> {
    /// Optional deformation of the uncropped, original-coordinate canvas.
    Liquify(&'a PhotoLiquifyNode),
    /// Optional final crop, orientation, mirror, and straighten node.
    Canvas(&'a PhotoCanvasNode),
}

/// The complete structural-node topology owned by one Recipe snapshot.
///
/// This is intentionally not a general node vector. Its shape encodes the
/// product rules directly: Liquify and Canvas are photo-private and may each
/// occur zero or one time; when present, execution order is always
/// `Liquify -> Canvas`. Grade Nodes remain the repeatable/shareable adjustment
/// system.
///
/// `canvas` retains the serialized Recipe v1 field name `geometry`. When this
/// value is flattened into [`super::RecipeSnapshot`], existing geometry JSON
/// therefore remains byte-shape compatible.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct PhotoStructuralNodes {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    liquify: Option<PhotoLiquifyNode>,
    #[serde(
        rename = "geometry",
        default,
        skip_serializing_if = "PhotoCanvasNode::is_identity"
    )]
    canvas: PhotoCanvasNode,
}

impl PhotoStructuralNodes {
    /// Creates and validates the fixed structural topology.
    ///
    /// # Errors
    ///
    /// Returns an error when the optional liquify or optional canvas node
    /// contains invalid authored state.
    pub fn new(
        liquify: Option<PhotoLiquifyNode>,
        canvas: PhotoCanvasNode,
    ) -> Result<Self, RecipeValidationError> {
        let nodes = Self { liquify, canvas };
        nodes.validate()?;
        Ok(nodes)
    }

    /// Creates the topology used by legacy Recipe v1 geometry callers.
    pub const fn with_canvas(canvas: PhotoCanvasNode) -> Self {
        Self {
            liquify: None,
            canvas,
        }
    }

    /// Returns the optional, photo-private liquify node.
    pub const fn liquify(&self) -> Option<&PhotoLiquifyNode> {
        self.liquify.as_ref()
    }

    /// Returns the fixed final-canvas slot, which may be absent from the
    /// authored processing stack.
    pub const fn canvas(&self) -> &PhotoCanvasNode {
        &self.canvas
    }

    /// Visits active structural nodes in their only legal execution order.
    pub fn iter(&self) -> impl Iterator<Item = PhotoStructuralNodeRef<'_>> {
        self.liquify
            .as_ref()
            .map(PhotoStructuralNodeRef::Liquify)
            .into_iter()
            .chain(
                self.canvas
                    .is_present()
                    .then_some(PhotoStructuralNodeRef::Canvas(&self.canvas)),
            )
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        if let Some(liquify) = &self.liquify {
            liquify.validate()?;
        }
        self.canvas.validate()
    }
}

#[cfg(test)]
mod tests;

//! Deterministic structural differences between immutable Recipe snapshots.
//!
//! A diff is derived, read-only data. It never rewrites either snapshot and
//! deliberately compares Recipe semantics instead of serialized Git text.
//! Follow [`model`] for the serialized exact-change records, [`compute`] for
//! identity-based comparison, and [`summary`] for UI-facing aggregate counts.

mod compute;
mod model;
mod summary;

pub use compute::diff_recipe_snapshots;
pub use model::{
    GraphDiff, IndexedLayer, LayerContentDiff, LayerContentKind, LayerInstanceDiff,
    LayerModification, LayerMove, NodeModification, RecipeDiff, SharedLayerDiff, ValueChange,
};
pub use summary::RecipeDiffSummary;

#[cfg(test)]
mod tests;

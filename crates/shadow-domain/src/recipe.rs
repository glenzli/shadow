//! Platform-neutral, non-destructive edit recipes and their immutable history.
//!
//! These types describe intent. They deliberately do not name a renderer,
//! database, UI toolkit, model provider, or concrete pixel implementation.
//!
//! This facade is the Recipe architecture index. Follow the responsibility
//! modules under `recipe/`: `snapshot` owns complete editable state and stable
//! identity; `history` owns immutable commits and refs; `edit_graph` owns the
//! typed adjustment DAG; `value` and `validation_error` own shared boundary
//! vocabulary; the remaining modules own their named photo-edit contracts.

mod edit_graph;
mod history;
mod input_settings;
mod layer;
mod local_mask;
mod photo_geometry;
mod retouch;
mod snapshot;
mod validation_error;
mod value;

#[cfg(test)]
mod test_support;

pub use edit_graph::{
    AdjustmentNode, EditGraph, GraphValidationError, ImageDomain, MaskCoordinateSpace, NodeInput,
    OperationDescriptor, ParameterBlock, ParameterValue, PortType, ProcessingStage,
};
pub use history::{NamedVersion, RecipeBranch, RecipeCommit, RecipeHistory};
pub use input_settings::{RecipeInputSettings, RecipeOpticsSettings};
pub use layer::{
    AdjustmentScope, BlendMode, LayerContent, LayerInstance, LayerRevision, LayerRevisionSelector,
};
pub use local_mask::{
    MAX_MASK_BRUSH_POINTS, MaskBrushPoint, MaskDefinition, MaskReference, MaskRevision,
};
pub use photo_geometry::{PhotoGeometry, PhotoQuarterTurn};
pub use retouch::{
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKE_POINTS, MAX_RETOUCH_STROKES_PER_RECIPE,
    RetouchMode, RetouchPoint, RetouchSpot, RetouchStroke,
};
pub use snapshot::{
    CURRENT_RECIPE_SCHEMA_VERSION, RecipeSnapshot, canonical_recipe_snapshot_digest,
};
pub use validation_error::RecipeValidationError;
pub use value::{BranchName, FiniteF64, OperationId, ParameterKey, UnitInterval, VersionName};

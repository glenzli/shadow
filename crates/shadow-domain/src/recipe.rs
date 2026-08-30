//! Platform-neutral, non-destructive edit recipes and their immutable history.
//!
//! These types describe intent. They deliberately do not name a renderer,
//! database, UI toolkit, model provider, or concrete pixel implementation.
//!
//! This facade is the Recipe architecture index. Follow the responsibility
//! modules under `recipe/`: `snapshot` owns complete editable state and stable
//! identity; `history` owns immutable commits and refs; `edit_graph` owns the
//! typed adjustment DAG; `value` and `validation_error` own shared boundary
//! vocabulary; `condition_mask` owns bounded pixel predicates and copy-only
//! presets; `mask_creation` freezes the current-node/new-node destination;
//! `photo_foundation` owns the mandatory source-development node;
//! `raw_foundation_denoise` owns its single-use, model-pinned AI RAW intent;
//! `photo_structural_nodes` fixes the photo-private Liquify/Canvas topology;
//! `photo_liquify` owns authored deformation gestures; `photo_geometry` owns
//! the mandatory final-canvas parameters; the remaining modules own their
//! named photo-edit contracts.

mod condition_mask;
mod edit_graph;
mod history;
mod image_completion;
mod input_settings;
mod layer;
mod local_mask;
mod mask_creation;
mod photo_foundation;
mod photo_geometry;
mod photo_liquify;
mod photo_structural_nodes;
mod portable;
mod raw_foundation_denoise;
mod retouch;
mod snapshot;
mod validation_error;
mod value;

#[cfg(test)]
mod test_support;

pub use condition_mask::{
    CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_LOWER, CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_UPPER,
    CURRENT_CONDITION_MASK_SCHEMA_VERSION, ConditionMaskExpression, ConditionMaskNode,
    ConditionMaskPredicate, ConditionMaskPreset, ConditionMaskReferenceError,
    ConditionMaskScalarSample, LOCAL_DETAIL_RESIDUAL_NORMALIZATION, LocalDetailAlgorithm,
    LocalDetailFullRenderScale, LocalDetailInput, MAX_CONDITION_MASK_BRANCHES,
    MAX_CONDITION_MASK_DEPTH, MAX_CONDITION_MASK_LEAVES, MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS,
    OKLCH_CHROMA_NORMALIZATION, local_detail_reference_response,
};
pub use edit_graph::{
    AdjustmentNode, EditGraph, GraphValidationError, ImageDomain, MaskCoordinateSpace, NodeInput,
    OperationDescriptor, ParameterBlock, ParameterValue, PortType, ProcessingStage,
};
pub use history::{NamedVersion, RecipeBranch, RecipeCommit, RecipeHistory};
pub use image_completion::{
    ImageCompletionRegion, MANAGED_IMAGE_COMPLETION_REFERENCE_VERSION,
    MAX_IMAGE_COMPLETION_PATCH_DIMENSION, MAX_IMAGE_COMPLETION_REGIONS_PER_RECIPE,
    ManagedImageCompletionPatch,
};
pub use input_settings::{RecipeInputSettings, RecipeOpticsSettings};
pub use layer::{
    AdjustmentScope, BlendMode, LayerContent, LayerInstance, LayerRevision, LayerRevisionSelector,
};
pub use local_mask::{
    MANAGED_RASTER_MASK_REFERENCE_VERSION, MAX_MANAGED_RASTER_MASK_DIMENSION,
    MAX_MASK_BRUSH_POINTS, MAX_SEMANTIC_MASK_QUERY_BYTES, MAX_SEMANTIC_MASK_REGIONS,
    ManagedRasterMask, MaskBrushPoint, MaskDefinition, MaskReference, MaskRevision,
    RasterMaskEncoding, SEMANTIC_MASK_INTENT_CONTRACT_VERSION, SemanticMaskAggregation,
    SemanticMaskIntent,
};
pub use mask_creation::{NodeLocalMaskCreationIntent, NodeLocalMaskCreationTarget};
pub use photo_foundation::{
    PhotoFoundationNode, RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN,
    RAW_WHITE_BALANCE_MAX_TEMPERATURE_KELVIN, RAW_WHITE_BALANCE_MAX_TINT,
    RAW_WHITE_BALANCE_MIN_TEMPERATURE_KELVIN, RAW_WHITE_BALANCE_MIN_TINT, RawTemperatureTint,
    RawWhiteBalance,
};
pub use photo_geometry::{PhotoCanvasNode, PhotoGeometry, PhotoQuarterTurn};
pub use photo_liquify::{
    LiquifyPoint, LiquifyStroke, MAX_LIQUIFY_POINTS_PER_STROKE, MAX_LIQUIFY_STROKES_PER_NODE,
    PhotoLiquifyNode,
};
pub use photo_structural_nodes::{PhotoStructuralNodeRef, PhotoStructuralNodes};
pub use portable::{
    CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION, MAX_SHADOW_RECIPE_DOCUMENT_BYTES,
    MAX_SHADOW_RECIPE_LABEL_BYTES, SHADOW_RECIPE_FORMAT, ShadowRecipeDocument,
    ShadowRecipeDocumentError,
};
pub use raw_foundation_denoise::{
    RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT, RawFoundationDenoise, RawFoundationDenoiseModel,
};
pub use retouch::{
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKE_POINTS, MAX_RETOUCH_STROKES_PER_RECIPE,
    RetouchMode, RetouchPoint, RetouchSpot, RetouchStroke,
};
pub use snapshot::{
    CURRENT_RECIPE_SCHEMA_VERSION, RecipeSnapshot, canonical_recipe_snapshot_digest,
};
pub use validation_error::RecipeValidationError;
pub use value::{BranchName, FiniteF64, OperationId, ParameterKey, UnitInterval, VersionName};

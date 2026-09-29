//! Stable domain types shared by Shadow's UI, catalog, renderer, and workers.
//!
//! This crate root is a public API and navigation facade:
//!
//! - `asset` owns durable asset locations and representation identity;
//! - `decision` owns photo rating/flag event contracts;
//! - `decode` owns provider-neutral RAW capability and preview snapshots;
//! - `edit_repository` indexes content identity, verified objects, Library
//!   state, and repository history contracts;
//! - `ids` owns strongly typed persistent identifiers;
//! - `operation` owns stable operation identifiers;
//! - `recipe` owns non-destructive edits and immutable history;
//! - `recipe_diff` owns semantic comparisons between Recipe snapshots.
//!
//! Follow each entry module for its responsibility map; substantive behavior
//! belongs there rather than in this facade.

mod asset;
mod decision;
mod decode;
mod edit_repository;
mod ids;
pub mod operation;
mod recipe;
mod recipe_diff;

pub use asset::{AssetLocation, LocationStatus, Platform, RepresentationKind};
pub use decision::{
    MAX_PHOTO_RATING, NewPhotoDecisionEvent, PhotoDecisionEvent, PhotoDecisionOrigin,
    PhotoDecisionState, PhotoDecisionValidationError, PhotoFlag,
};
pub use decode::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    FocusObservationSnapshot, FocusObservationSource, GpsMetadataSnapshot, ImageDimensions,
    ImageMargins, PendingCorrectionsSnapshot, PreviewByteOrder, PreviewCodec,
    PreviewDescriptorSnapshot, PreviewPayload, ProxyPayload, RawDevelopmentCapabilitySnapshot,
    RawMetadataSnapshot,
};
pub use edit_repository::{
    EditCommitId, EditEntityChangeV1, EditEntityEntryV1, EditEntityMapV1, EditObject,
    EditObjectEdge, EditObjectId, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryError, EditRepositoryRefExpectation,
    EditRepositoryRefKind, LibraryRootV1,
};
pub use ids::{
    BranchId, CollectionId, EntityId, GradeNodeId, GroupId, ImportSessionId, KeywordId, LayerId,
    LayerInstanceId, LayerRevisionId, LibrarySourceId, LocationId, MaskComponentId, MaskId, NodeId,
    OutputTargetId, PhotoId, PhotoVariantId, RecipeCommitId, RecipeId, RepresentationId,
    SelectionId, ShootId, StyleId, VersionId,
};
pub use recipe::{
    AdjustmentNode, AdjustmentScope, BlendMode, BranchName,
    CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_LOWER, CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_UPPER,
    CURRENT_CONDITION_MASK_SCHEMA_VERSION, CURRENT_RECIPE_SCHEMA_VERSION,
    CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION, ConditionMaskExpression, ConditionMaskNode,
    ConditionMaskPredicate, ConditionMaskPreset, ConditionMaskReferenceError,
    ConditionMaskScalarSample, EditGraph, FiniteF64, GraphValidationError, ImageCompletionRegion,
    ImageDomain, LOCAL_DETAIL_RESIDUAL_NORMALIZATION, LayerContent, LayerInstance, LayerRevision,
    LayerRevisionSelector, LiquifyPoint, LiquifyStroke, LocalDetailAlgorithm,
    LocalDetailFullRenderScale, LocalDetailInput, MANAGED_IMAGE_COMPLETION_REFERENCE_VERSION,
    MANAGED_RASTER_MASK_REFERENCE_VERSION, MAX_CONDITION_MASK_BRANCHES, MAX_CONDITION_MASK_DEPTH,
    MAX_CONDITION_MASK_LEAVES, MAX_IMAGE_COMPLETION_PATCH_DIMENSION,
    MAX_IMAGE_COMPLETION_REGIONS_PER_RECIPE, MAX_LIQUIFY_POINTS_PER_STROKE,
    MAX_LIQUIFY_STROKES_PER_NODE, MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS,
    MAX_MANAGED_RASTER_MASK_DIMENSION, MAX_MASK_BRUSH_POINTS, MAX_MASK_COMPONENTS,
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKE_POINTS, MAX_RETOUCH_STROKES_PER_RECIPE,
    MAX_SEMANTIC_MASK_QUERY_BYTES, MAX_SEMANTIC_MASK_REGIONS, MAX_SHADOW_RECIPE_DOCUMENT_BYTES,
    MAX_SHADOW_RECIPE_LABEL_BYTES, ManagedImageCompletionPatch, ManagedRasterMask, MaskBrushPoint,
    MaskComponent, MaskComponentOperation, MaskComposite, MaskCoordinateSpace, MaskDefinition,
    MaskReference, MaskRevision, NamedVersion, NodeInput, NodeLocalMaskCreationIntent,
    NodeLocalMaskCreationTarget, OKLCH_CHROMA_NORMALIZATION, OperationDescriptor, OperationId,
    ParameterBlock, ParameterKey, ParameterValue, PhotoCanvasNode, PhotoFoundationNode,
    PhotoGeometry, PhotoLiquifyNode, PhotoQuarterTurn, PhotoStructuralNodeRef,
    PhotoStructuralNodes, PortType, ProcessingStage, RAW_FOUNDATION_DENOISE_FULL_AMOUNT_PERCENT,
    RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN, RAW_WHITE_BALANCE_MAX_TEMPERATURE_KELVIN,
    RAW_WHITE_BALANCE_MAX_TINT, RAW_WHITE_BALANCE_MIN_TEMPERATURE_KELVIN,
    RAW_WHITE_BALANCE_MIN_TINT, RasterMaskEncoding, RawFoundationDenoise,
    RawFoundationDenoiseModel, RawTemperatureTint, RawWhiteBalance, RecipeBranch, RecipeCommit,
    RecipeHistory, RecipeInputSettings, RecipeOpticsSettings, RecipeSnapshot,
    RecipeValidationError, RetouchMode, RetouchPoint, RetouchSpot, RetouchStroke,
    SEMANTIC_MASK_INTENT_CONTRACT_VERSION, SHADOW_RECIPE_FORMAT, SemanticMaskAggregation,
    SemanticMaskIntent, ShadowRecipeDocument, ShadowRecipeDocumentError, ShadowRecipeLutResource,
    UnitInterval, VersionName, canonical_recipe_snapshot_digest, local_detail_reference_response,
};
pub use recipe_diff::{
    GraphDiff, IndexedLayer, LayerContentDiff, LayerContentKind, LayerInstanceDiff,
    LayerModification, LayerMove, NodeModification, RecipeDiff, RecipeDiffSummary, SharedLayerDiff,
    ValueChange, diff_recipe_snapshots,
};

pub use recipe::{
    MAX_PAINT_LAYERS, MAX_PAINT_POINTS, MAX_PAINT_STROKES, PaintBlendMode, PaintLayer, PaintPoint,
    PaintStroke,
};

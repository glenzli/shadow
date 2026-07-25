//! Stable domain types shared by Shadow's UI, catalog, renderer, and workers.

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
    ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PreviewByteOrder, PreviewCodec,
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
    BranchId, CollectionId, EntityId, GradeNodeId, GroupId, ImportSessionId, LayerId,
    LayerInstanceId, LayerRevisionId, LibrarySourceId, LocationId, MaskId, NodeId, OutputTargetId,
    PhotoId, RecipeCommitId, RecipeId, RepresentationId, SelectionId, ShootId, StyleId, VersionId,
};
pub use recipe::{
    AdjustmentNode, AdjustmentScope, BlendMode, BranchName, CURRENT_RECIPE_SCHEMA_VERSION,
    EditGraph, FiniteF64, GraphValidationError, ImageDomain, LayerContent, LayerInstance,
    LayerRevision, LayerRevisionSelector, MAX_RETOUCH_SPOTS_PER_RECIPE, MaskCoordinateSpace,
    MaskDefinition, MaskReference, MaskRevision, NamedVersion, NodeInput, OperationDescriptor,
    OperationId, ParameterBlock, ParameterKey, ParameterValue, PhotoGeometry, PhotoQuarterTurn,
    PortType, ProcessingStage, RecipeBranch, RecipeCommit, RecipeHistory, RecipeInputSettings,
    RecipeOpticsSettings, RecipeSnapshot, RecipeValidationError, RetouchSpot, UnitInterval,
    VersionName, canonical_recipe_snapshot_digest,
};
pub use recipe_diff::{
    GraphDiff, IndexedLayer, LayerContentDiff, LayerContentKind, LayerInstanceDiff,
    LayerModification, LayerMove, NodeModification, RecipeDiff, RecipeDiffSummary, SharedLayerDiff,
    ValueChange, diff_recipe_snapshots,
};

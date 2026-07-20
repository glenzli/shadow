//! Stable domain types shared by Shadow's UI, catalog, renderer, and workers.

mod asset;
mod decode;
mod ids;
pub mod operation;
mod recipe;
mod recipe_diff;

pub use asset::{AssetLocation, LocationStatus, Platform, RepresentationKind};
pub use decode::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PreviewByteOrder, PreviewCodec,
    PreviewDescriptorSnapshot, PreviewPayload, ProxyPayload, RawMetadataSnapshot,
};
pub use ids::{
    BranchId, CollectionId, EntityId, GroupId, ImportSessionId, LayerId, LayerInstanceId,
    LayerRevisionId, LocationId, MaskId, NodeId, OutputTargetId, PhotoId, RecipeCommitId, RecipeId,
    RepresentationId, SelectionId, ShootId, StyleId, VersionId,
};
pub use recipe::{
    AdjustmentNode, AdjustmentScope, BlendMode, BranchName, CURRENT_RECIPE_SCHEMA_VERSION,
    EditGraph, FiniteF64, GraphValidationError, ImageDomain, LayerContent, LayerInstance,
    LayerRevision, LayerRevisionSelector, MaskCoordinateSpace, MaskReference, NamedVersion,
    NodeInput, OperationDescriptor, OperationId, ParameterBlock, ParameterKey, ParameterValue,
    PortType, ProcessingStage, RecipeBranch, RecipeCommit, RecipeHistory, RecipeSnapshot,
    RecipeValidationError, UnitInterval, VersionName,
};
pub use recipe_diff::{
    GraphDiff, IndexedLayer, LayerContentDiff, LayerContentKind, LayerInstanceDiff,
    LayerModification, LayerMove, NodeModification, RecipeDiff, RecipeDiffSummary, SharedLayerDiff,
    ValueChange, diff_recipe_snapshots,
};

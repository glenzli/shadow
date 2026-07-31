//! Shared Recipe validation vocabulary exposed across responsibility boundaries.

use thiserror::Error;

use crate::{
    BranchId, LayerInstanceId, LayerRevisionId, MaskId, NodeId, RecipeCommitId, RecipeId, VersionId,
};

use super::{BranchName, GraphValidationError, VersionName};

#[derive(Debug, Clone, PartialEq, Error)]
pub enum RecipeValidationError {
    #[error("persisted floating-point values must be finite")]
    NonFiniteNumber,
    #[error("value {0} is outside the inclusive range [0, 1]")]
    UnitIntervalOutOfRange(f64),
    #[error("{kind} {reason}")]
    InvalidText {
        kind: &'static str,
        reason: &'static str,
    },
    #[error("{kind} is {actual_bytes} bytes; maximum is {max_bytes}")]
    TextTooLong {
        kind: &'static str,
        max_bytes: usize,
        actual_bytes: usize,
    },
    #[error("parameter schema version must be non-zero")]
    ZeroParameterSchemaVersion,
    #[error("operation has {0} inputs, which exceeds the fixed-width u16 port index")]
    TooManyPorts(usize),
    #[error("node {node_id} expects {expected} inputs but has {actual}")]
    NodeInputArity {
        node_id: NodeId,
        expected: usize,
        actual: usize,
    },
    #[error("mask revision must be non-zero")]
    ZeroMaskRevision,
    #[error("linear-gradient mask start and end must not be the same point")]
    DegenerateLinearMask,
    #[error("radial-gradient mask radii must both be greater than zero")]
    DegenerateRadialMask,
    #[error("brush mask radius must be greater than zero")]
    DegenerateBrushMask,
    #[error("brush mask contains {0} points, but at most 4096 are supported")]
    TooManyMaskBrushPoints(usize),
    #[error("luminance-range mask lower bound {lower} must not exceed upper bound {upper}")]
    InvalidLuminanceMaskRange { lower: f64, upper: f64 },
    #[error("color-range mask hue {0}° must use the canonical interval [0, 360)")]
    InvalidColorMaskHue(f64),
    #[error("color-range mask width {0}° must be between 1° and 180°")]
    InvalidColorMaskWidth(f64),
    #[error("condition-mask expression supports schema {expected}, received {actual}")]
    UnsupportedConditionMaskSchemaVersion { expected: u32, actual: u32 },
    #[error("condition-mask expression depth {0} exceeds the fixed maximum of 4")]
    ConditionMaskDepthExceeded(usize),
    #[error("condition-mask expression contains {0} leaves, but at most 8 are supported")]
    TooManyConditionMaskLeaves(usize),
    #[error(
        "condition-mask {operator} operator contains {count} children; expected between 2 and 4"
    )]
    InvalidConditionMaskBranchCount {
        operator: &'static str,
        count: usize,
    },
    #[error("condition-mask {kind} lower bound {lower} must not exceed upper bound {upper}")]
    InvalidConditionMaskRange {
        kind: &'static str,
        lower: f64,
        upper: f64,
    },
    #[error("condition-mask hue {0}° must use the canonical interval [0, 360)")]
    InvalidConditionMaskHue(f64),
    #[error("condition-mask hue half-width {0}° must be between 1° and 180°")]
    InvalidConditionMaskHueWidth(f64),
    #[error(
        "condition-mask minimum-chroma feather must be zero when the minimum-chroma gate is disabled"
    )]
    UnusedConditionMaskChromaFeather,
    #[error("condition-mask local-detail radius {0} must be between 1 and 64 level-zero pixels")]
    InvalidLocalDetailMaskRadius(u16),
    #[error("new local-mask destination node {0} cannot also be its insertion anchor")]
    SelfAnchoredMaskDestination(LayerInstanceId),
    #[error(
        "a single executable condition leaf must use the legacy luminance/color mask representation"
    )]
    NonCanonicalConditionMaskExpression,
    #[error(
        "managed raster-mask reference supports contract {expected}, received contract {actual}"
    )]
    UnsupportedManagedRasterMaskReferenceVersion { expected: u32, actual: u32 },
    #[error("managed raster-mask storage revision must be non-zero")]
    ZeroManagedRasterMaskStorageRevision,
    #[error("managed raster-mask content identity must be a lowercase BLAKE3 digest")]
    InvalidManagedRasterMaskContentHash,
    #[error("managed raster-mask object identity is not canonical for its revision and digest")]
    InvalidManagedRasterMaskStoreObjectId,
    #[error("managed raster-mask {kind} extent must be non-zero")]
    EmptyManagedRasterMaskExtent { kind: &'static str },
    #[error("managed raster-mask {kind} extent {width}x{height} exceeds the supported bound")]
    ManagedRasterMaskExtentTooLarge {
        kind: &'static str,
        width: u32,
        height: u32,
    },
    #[error(
        "managed raster-mask byte length does not match its tightly packed extent: expected {expected}, got {actual}"
    )]
    ManagedRasterMaskByteLengthMismatch { expected: u64, actual: u64 },
    #[error("managed raster-mask expansion {0}% must be between -100% and 100%")]
    InvalidManagedRasterMaskExpansion(i8),
    #[error("managed raster-mask feather {0}% must be between 0% and 100%")]
    InvalidManagedRasterMaskFeather(u8),
    #[error("mask {mask_id} revision {revision} appears more than once")]
    DuplicateMaskRevision { mask_id: MaskId, revision: u32 },
    #[error("retouch spot radius {0} must be between 1 and 128 full-resolution pixels")]
    InvalidRetouchSpotRadius(u16),
    #[error("retouch stroke must contain at least one point")]
    EmptyRetouchStroke,
    #[error("retouch stroke contains {0} points, but at most 512 are supported")]
    TooManyRetouchStrokePoints(usize),
    #[error("retouch stroke radius {0} must be between 1 and 128 full-resolution pixels")]
    InvalidRetouchStrokeRadius(u16),
    #[error("retouch donor source offset must stay within eight brush radii")]
    InvalidRetouchSourceOffset,
    #[error("Recipe contains {0} retouch spots, but at most 64 are supported")]
    TooManyRetouchSpots(usize),
    #[error("Recipe contains {0} retouch strokes, but at most 64 are supported")]
    TooManyRetouchStrokes(usize),
    #[error("photo crop must retain non-zero width and height")]
    DegeneratePhotoCrop,
    #[error("photo straighten angle {0}° is outside the supported -45°..45° range")]
    InvalidPhotoStraightenDegrees(f64),
    #[error("a persisted liquify node must contain at least one gesture")]
    EmptyLiquifyNode,
    #[error("liquify push gesture contains {0} points; at least two are required")]
    TooFewLiquifyStrokePoints(usize),
    #[error("liquify reconstruct gesture must contain at least one point")]
    EmptyLiquifyReconstruct,
    #[error("liquify gesture contains {0} points, but at most 2048 are supported")]
    TooManyLiquifyStrokePoints(usize),
    #[error("liquify gesture must contain pressured movement")]
    DegenerateLiquifyStroke,
    #[error("liquify brush radius must be greater than zero")]
    DegenerateLiquifyBrushRadius,
    #[error("liquify brush strength must be greater than zero")]
    DegenerateLiquifyStrength,
    #[error("liquify reconstruct gesture requires earlier deformation")]
    LiquifyReconstructWithoutPriorDeformation,
    #[error("liquify node contains {0} gestures, but at most 128 are supported")]
    TooManyLiquifyStrokes(usize),
    #[error("inline layer {layer_id} must have PHOTO scope")]
    InlineLayerMustBePhotoScoped { layer_id: LayerInstanceId },
    #[error("layer revision must be non-zero")]
    ZeroLayerRevision,
    #[error("layer revision {0} cannot parent itself")]
    SelfParentLayerRevision(LayerRevisionId),
    #[error(
        "layer revision {revision_number} has invalid parent presence (has_parent={has_parent})"
    )]
    InvalidLayerRevisionParent {
        revision_number: u32,
        has_parent: bool,
    },
    #[error("recipe schema version must be non-zero")]
    ZeroRecipeSchemaVersion,
    #[error("RAW white-balance temperature {0} K is outside the supported 2000..=25000 K range")]
    InvalidRawWhiteBalanceTemperature(u32),
    #[error("RAW white-balance tint {0} is outside the supported -150..=150 range")]
    InvalidRawWhiteBalanceTint(i16),
    #[error("AI RAW denoise amount {0}% is outside the supported 0..=100% range")]
    InvalidRawFoundationDenoiseAmount(u8),
    #[error("manual optics profile must identify both a camera and a lens")]
    IncompleteOpticsProfile,
    #[error("{kind} is {value}; expected an integer in [-100, 100]")]
    InvalidOpticsManualValue { kind: &'static str, value: i16 },
    #[error("manual optical-vignetting midpoint is {0}; expected an integer in [0, 100]")]
    InvalidOpticsVignettingMidpoint(u8),
    #[error("layer instance {0} appears more than once")]
    DuplicateLayerInstance(LayerInstanceId),
    #[error("layer {0} follows a mutable shared head and must be pinned before commit")]
    UnresolvedSharedLayer(LayerInstanceId),
    #[error("recipe commit has {0} parents; at most two are supported")]
    TooManyCommitParents(usize),
    #[error("recipe commit {0} cannot parent itself")]
    SelfParentCommit(RecipeCommitId),
    #[error("recipe commit parent {0} appears more than once")]
    DuplicateCommitParent(RecipeCommitId),
    #[error("recipe history must contain at least one commit")]
    EmptyHistory,
    #[error("expected recipe {expected}, found {actual}")]
    RecipeIdMismatch {
        expected: RecipeId,
        actual: RecipeId,
    },
    #[error("recipe commit {0} appears more than once")]
    DuplicateCommit(RecipeCommitId),
    #[error("recipe history must have exactly one root commit; found {0}")]
    InvalidRootCommitCount(usize),
    #[error("commit {commit_id} references unknown parent {parent_id}")]
    UnknownCommitParent {
        commit_id: RecipeCommitId,
        parent_id: RecipeCommitId,
    },
    #[error("recipe commit history contains a cycle through {0}")]
    CommitCycle(RecipeCommitId),
    #[error("branch {0} appears more than once")]
    DuplicateBranch(BranchId),
    #[error("branch name {0:?} appears more than once")]
    DuplicateBranchName(BranchName),
    #[error("named version {0} appears more than once")]
    DuplicateVersion(VersionId),
    #[error("version name {0:?} appears more than once")]
    DuplicateVersionName(VersionName),
    #[error("recipe ref targets unknown commit {0}")]
    UnknownRefTarget(RecipeCommitId),
    #[error(transparent)]
    Graph(#[from] GraphValidationError),
}

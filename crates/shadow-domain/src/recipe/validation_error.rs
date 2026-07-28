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

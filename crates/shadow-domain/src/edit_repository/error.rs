//! Stable edit-repository validation and reconstruction failures.

use super::{
    content_id::{EditCommitId, EditObjectId},
    repository_object::EditObjectKind,
};

#[derive(Debug, thiserror::Error)]
pub enum EditRepositoryError {
    #[error("invalid 256-bit edit digest: {0:?}")]
    InvalidDigest(String),
    #[error("edit object format versions start at one")]
    InvalidFormatVersion,
    #[error("unknown edit object kind: {0}")]
    UnknownObjectKind(String),
    #[error("unknown edit repository ref kind: {0}")]
    UnknownRefKind(String),
    #[error("edit object {id} does not match its content digest")]
    ObjectDigestMismatch { id: EditObjectId },
    #[error("edit object payload is not canonical JSON")]
    NonCanonicalObjectPayload,
    #[error("unsupported {kind:?} edit object format version {format_version}")]
    UnsupportedObjectFormatVersion {
        kind: EditObjectKind,
        format_version: u32,
    },
    #[error(
        "edit object type mismatch: expected {expected_kind:?} v{expected_version}, got {actual_kind:?} v{actual_version}"
    )]
    ObjectTypeMismatch {
        expected_kind: EditObjectKind,
        expected_version: u32,
        actual_kind: EditObjectKind,
        actual_version: u32,
    },
    #[error("edit commit {id} does not match its content digest")]
    CommitDigestMismatch { id: EditCommitId },
    #[error("edit commit payload is not canonical JSON")]
    NonCanonicalCommitPayload,
    #[error("edit object contains duplicate edge role/position")]
    DuplicateObjectEdge,
    #[error("edit object edges do not match its typed payload")]
    ObjectEdgesDoNotMatchPayload,
    #[error("invalid legacy Recipe edit object: {0}")]
    InvalidLegacyRecipe(String),
    #[error("invalid shared Grade Node revision: {0}")]
    InvalidGradeNodeRevision(String),
    #[error("invalid edit object edge role: {0:?}")]
    InvalidEdgeRole(String),
    #[error("invalid edit entity key: {0:?}")]
    InvalidEntityKey(String),
    #[error("duplicate or unsorted edit entity key: {0:?}")]
    DuplicateOrUnsortedEntityKey(String),
    #[error("edit entity map is too large")]
    TooManyEntityEntries,
    #[error("edit repository commit has too many parents")]
    TooManyCommitParents,
    #[error("edit repository commit repeats one parent")]
    DuplicateCommitParent,
    #[error("edit repository commit message is invalid")]
    InvalidCommitMessage,
    #[error("edit repository JSON error: {0}")]
    Json(#[from] serde_json::Error),
}

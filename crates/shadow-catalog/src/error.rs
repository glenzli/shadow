//! Unified public failure contract for catalog persistence and validation.

use shadow_domain::{KeywordId, PhotoFlag, PhotoId, RepresentationId};
use thiserror::Error;

use crate::{export_queue, recipe::RecipeRefExpectation};

#[derive(Debug, Error)]
pub enum CatalogError {
    #[error("SQLite catalog error: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error(
        "development catalog reset required: found schema {found:?}; Shadow currently supports only a fresh catalog schema v1"
    )]
    DevelopmentCatalogResetRequired { found: Option<i64> },
    #[error("import session {0} does not exist")]
    ImportSessionNotFound(shadow_domain::ImportSessionId),
    #[error("import session {id} cannot be used while state is {state}")]
    InvalidImportSessionState {
        id: shadow_domain::ImportSessionId,
        state: &'static str,
    },
    #[error("import entry is missing in session {session_id}: {display_path}")]
    ImportEntryNotFound {
        session_id: shadow_domain::ImportSessionId,
        display_path: String,
    },
    #[error(
        "cannot attach verified relocation because the target location is already registered: {display_path}"
    )]
    RelinkTargetLocationAlreadyRegistered { display_path: String },
    #[error(
        "cannot attach verified relocation because its exact identity is not recorded for expected representation {expected_representation_id}"
    )]
    RelinkIdentityNotRecorded {
        expected_representation_id: RepresentationId,
    },
    #[error(
        "cannot attach verified relocation because its exact identity belongs to representation {actual_representation_id}, not expected representation {expected_representation_id}"
    )]
    RelinkIdentityOwnerMismatch {
        expected_representation_id: RepresentationId,
        actual_representation_id: RepresentationId,
    },
    #[error(
        "representation {representation_id} changed while its content identity was being bound"
    )]
    ContentIdentitySourceChanged { representation_id: RepresentationId },
    #[error("cannot start catalog writer actor: {0}")]
    ActorStart(#[source] std::io::Error),
    #[error("catalog writer actor is unavailable")]
    ActorUnavailable,
    #[error("catalog writer actor panicked")]
    ActorPanicked,
    #[error("representation {0} does not exist")]
    RepresentationNotFound(RepresentationId),
    #[error("photo {0} does not exist")]
    PhotoNotFound(PhotoId),
    #[error("album {0} does not exist")]
    AlbumNotFound(shadow_domain::CollectionId),
    #[error("Library keyword {0} does not exist")]
    LibraryKeywordNotFound(KeywordId),
    #[error("invalid content identity: {0}")]
    InvalidContentIdentity(String),
    #[error("invalid Library metadata facts: {0}")]
    InvalidLibraryFacts(String),
    #[error("invalid Library photo state: {0}")]
    InvalidLibraryState(String),
    #[error("invalid Library album: {0}")]
    InvalidAlbum(String),
    #[error("invalid Library keyword: {0}")]
    InvalidLibraryKeyword(String),
    #[error("invalid Library query: {0}")]
    InvalidLibraryQuery(String),
    #[error("invalid export queue data: {0}")]
    InvalidExport(String),
    #[error("export preset {0} does not exist")]
    ExportPresetNotFound(export_queue::ExportPresetId),
    #[error("export preset revision {0} does not exist")]
    ExportPresetRevisionNotFound(export_queue::ExportPresetRevisionId),
    #[error("export job {0} does not exist")]
    ExportJobNotFound(export_queue::ExportJobId),
    #[error("export item {0} does not exist")]
    ExportItemNotFound(export_queue::ExportItemId),
    #[error(
        "export item {item_id} state did not match expected {expected:?}; actual state is {actual:?}"
    )]
    ExportItemStateMismatch {
        item_id: export_queue::ExportItemId,
        expected: export_queue::ExportItemState,
        actual: export_queue::ExportItemState,
    },
    #[error("export item {item_id} cannot transition from {from:?} to {to:?}")]
    InvalidExportTransition {
        item_id: export_queue::ExportItemId,
        from: export_queue::ExportItemState,
        to: export_queue::ExportItemState,
    },
    #[error("representation {representation_id} is not owned by export photo {photo_id}")]
    ExportRepresentationOwnerMismatch {
        photo_id: PhotoId,
        representation_id: RepresentationId,
    },
    #[error("export recipe snapshot digest does not match immutable commit {commit_id}")]
    ExportRecipeSnapshotMismatch {
        commit_id: shadow_domain::RecipeCommitId,
    },
    #[error("invalid decode snapshot: {0}")]
    InvalidDecodeSnapshot(&'static str),
    #[error("decode snapshot field {field} is outside SQLite's integer range")]
    DecodeSnapshotValueOutOfRange { field: &'static str },
    #[error("unsupported persisted decode snapshot schema {0}")]
    UnsupportedDecodeSnapshotSchema(i64),
    #[error("decode snapshot JSON error: {0}")]
    DecodeSnapshotJson(#[from] serde_json::Error),
    #[error("invalid cached artifact: {0}")]
    InvalidCachedArtifact(&'static str),
    #[error("cached artifact field {field} is outside SQLite's integer range")]
    CachedArtifactValueOutOfRange { field: &'static str },
    #[error("unknown persisted cached artifact {field}: {value}")]
    UnknownCachedArtifactValue { field: &'static str, value: String },
    #[error("unknown persisted platform: {0}")]
    UnknownPlatform(String),
    #[error("invalid Recipe: {0}")]
    InvalidRecipe(String),
    #[error("Recipe JSON error: {0}")]
    RecipeJson(serde_json::Error),
    #[error("Recipe commit {0} does not exist")]
    RecipeCommitNotFound(shadow_domain::RecipeCommitId),
    #[error("Recipe commit {0} already exists and cannot be overwritten")]
    RecipeCommitAlreadyExists(shadow_domain::RecipeCommitId),
    #[error("Recipe commit {commit_id} is not owned by photo {photo_id}")]
    RecipeCommitOwnerMismatch {
        photo_id: PhotoId,
        commit_id: shadow_domain::RecipeCommitId,
    },
    #[error("invalid Recipe ref name: {0:?}")]
    InvalidRecipeRefName(String),
    #[error("Recipe ref name {0:?} appears more than once in one commit")]
    DuplicateRecipeRefName(String),
    #[error(
        "Recipe ref {name:?} for photo {photo_id} did not match expectation {expected:?}; current commit is {actual:?}"
    )]
    RecipeRefExpectationMismatch {
        photo_id: PhotoId,
        name: String,
        expected: RecipeRefExpectation,
        actual: Option<shadow_domain::RecipeCommitId>,
    },
    #[error("unknown persisted Recipe ref kind: {0}")]
    UnknownRecipeRefKind(String),
    #[error("invalid AI feedback: {0}")]
    InvalidFeedback(String),
    #[error("AI feedback event id {0:?} already exists and cannot be overwritten")]
    FeedbackEventAlreadyExists(String),
    #[error("AI feedback event id {0:?} does not exist")]
    FeedbackEventNotFound(String),
    #[error(
        "presented visual representation {representation_id} is not owned by candidate photo {photo_id}"
    )]
    FeedbackVisualRepresentationOwnerMismatch {
        photo_id: PhotoId,
        representation_id: RepresentationId,
    },
    #[error("AI feedback forget fact id {0:?} already exists and cannot be overwritten")]
    FeedbackForgetFactAlreadyExists(String),
    #[error("AI feedback page limit {limit} is outside 1 through {maximum}")]
    InvalidFeedbackPageLimit { limit: usize, maximum: usize },
    #[error("AI feedback sequence space is exhausted")]
    FeedbackSequenceExhausted,
    #[error("AI feedback JSON error: {0}")]
    FeedbackJson(serde_json::Error),
    #[error("persisted AI feedback failed its integrity check: {0}")]
    InvalidPersistedFeedback(&'static str),
    #[error("invalid photo decision: {0}")]
    InvalidPhotoDecision(String),
    #[error("photo decision event id {0:?} already exists and cannot be overwritten")]
    PhotoDecisionEventAlreadyExists(String),
    #[error(
        "photo decision head for {photo_id} did not match expected sequence {expected}; current sequence is {actual}"
    )]
    PhotoDecisionHeadMismatch {
        photo_id: PhotoId,
        expected: u64,
        actual: u64,
    },
    #[error(
        "photo decision before-state for {photo_id} disagrees with head {head_sequence}: expected {expected_flag:?}/{expected_rating}, current {actual_flag:?}/{actual_rating}"
    )]
    PhotoDecisionBeforeStateMismatch {
        photo_id: PhotoId,
        head_sequence: u64,
        expected_flag: PhotoFlag,
        expected_rating: u8,
        actual_flag: PhotoFlag,
        actual_rating: u8,
    },
    #[error("photo decision history page limit {limit} is outside 1 through {maximum}")]
    InvalidPhotoDecisionPageLimit { limit: usize, maximum: usize },
    #[error("photo decision sequence space is exhausted")]
    PhotoDecisionSequenceExhausted,
    #[error("photo decision JSON error: {0}")]
    PhotoDecisionJson(serde_json::Error),
    #[error("persisted photo decision failed its integrity check: {0}")]
    InvalidPersistedPhotoDecision(&'static str),
    #[error("invalid technical observation: {0}")]
    InvalidTechnicalObservation(String),
    #[error("technical observation JSON error: {0}")]
    TechnicalObservationJson(serde_json::Error),
    #[error("technical observation field {field} is outside SQLite's integer range")]
    TechnicalObservationValueOutOfRange { field: &'static str },
    #[error("persisted technical observation failed its integrity check: {0}")]
    InvalidPersistedTechnicalObservation(&'static str),
    #[error("invalid edit repository object: {0}")]
    InvalidEditObject(String),
    #[error("edit object {0} does not exist")]
    EditObjectNotFound(shadow_domain::EditObjectId),
    #[error("content-addressed edit object {0} conflicts with persisted bytes or edges")]
    EditObjectCollision(shadow_domain::EditObjectId),
    #[error("invalid edit repository commit: {0}")]
    InvalidEditRepositoryCommit(String),
    #[error("edit repository commit {0} does not exist")]
    EditRepositoryCommitNotFound(shadow_domain::EditCommitId),
    #[error("content-addressed edit repository commit {0} conflicts with persisted bytes")]
    EditRepositoryCommitCollision(shadow_domain::EditCommitId),
    #[error("invalid edit repository ref name: {0:?}")]
    InvalidEditRepositoryRefName(String),
    #[error("edit repository ref name {0:?} appears more than once in one commit")]
    DuplicateEditRepositoryRefName(String),
    #[error(
        "edit repository ref {name:?} did not match expectation {expected:?}; current commit is {actual:?}"
    )]
    EditRepositoryRefExpectationMismatch {
        name: String,
        expected: shadow_domain::EditRepositoryRefExpectation,
        actual: Option<shadow_domain::EditCommitId>,
    },
    #[error("unknown persisted edit repository ref kind: {0}")]
    UnknownEditRepositoryRefKind(String),
}

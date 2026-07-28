//! SQLite-backed catalog persistence.
//!
//! This crate owns the current development schema and write transactions. It deliberately
//! knows nothing about Qt, RAW decoding, or render jobs.
//!
//! Start with [`catalog`] for connection lifecycle, [`asset_registration`] for idempotent
//! source registration, [`schema_v1`] for the on-disk shape, [`writer`] for serialized
//! mutation dispatch, and the responsibility-named repository modules below for feature
//! reads and transactions.

mod asset_registration;
mod backup;
mod cache_artifact;
mod catalog;
mod decision;
mod decode_snapshot;
mod edit_repository;
mod error;
mod export_queue;
mod feedback;
mod import_journal;
mod library;
mod library_metadata;
mod recipe;
mod review;
mod row_codec;
mod schema_v1;
mod store;
mod technical_observation;
mod writer;

pub use asset_registration::{RegisterAsset, RegisteredAsset, RegistrationStatus};
pub use backup::{
    CatalogBackupError, CatalogBackupReceipt, CatalogBackupVerification, create_catalog_backup,
    verify_catalog_backup,
};
pub use cache_artifact::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRecord, CachedArtifactRole,
    InvalidateCachedArtifactStatus, LiveCachedArtifactBlob, RecordCachedArtifact,
    RecordCachedArtifactStatus,
};
pub use catalog::{Catalog, CatalogStats};
pub use decision::{MAX_PHOTO_DECISION_PAGE_SIZE, PhotoDecisionPage};
pub use decode_snapshot::{
    DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};
pub use edit_repository::{
    CommitEditRepository, CommitRecipeAndEditRepository, CommitRecipeAndEditRepositoryResult,
    EditObjectPackWrite, EditObjectRecord, EditRepositoryCommitRecord, EditRepositoryRefRecord,
    EditRepositoryRefUpdate, StoreEditObjectPackResult,
};
pub use error::CatalogError;
pub use export_queue::{
    AdvanceExportItem, EnqueueExportJob, ExportFailure, ExportItemId, ExportItemRecord,
    ExportItemState, ExportJobId, ExportJobProgress, ExportJobRecord, ExportJobState,
    ExportOutputReceiptId, ExportOutputReceiptRecord, ExportPresetId, ExportPresetRecord,
    ExportPresetRevisionId, ExportPresetRevisionRecord, ExportQueueRecovery, ExportSettingsSource,
    MAX_EXPORT_JOB_PAGE_SIZE, NewExportItem, NewExportOutputReceipt,
};
pub use feedback::{FeedbackPage, MAX_FEEDBACK_PAGE_SIZE};
pub use import_journal::{
    ImportSession, ImportSessionState, ImportSessionSummary, SourceScanReconciliation,
};
pub use library::{
    AlbumKind, AlbumRecord, ContentIdentity, ContentIdentityScope, LibraryApertureRange,
    LibraryDateRange, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryFacetValue,
    LibraryPhotoCursor, LibraryPhotoFacts, LibraryPhotoFilter, LibraryPhotoPage,
    LibraryPhotoRecord, LibrarySourceHealth, LibrarySourceRecord, MAX_LIBRARY_FACET_PAGE_SIZE,
    MAX_LIBRARY_PAGE_SIZE, MissingSourceLocationCursor, MissingSourceLocationPage,
    MissingSourceLocationRecord, MissingSourceRelinkTarget, PhotoLibraryState,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RelinkMatch,
    SetPhotoLibraryState, SmartAlbumQueryV1, library_equipment_key,
};
pub use recipe::{
    CommitRecipe, RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind, RecipeRefRecord,
    RecipeRefTarget, SetRecipeRef,
};
pub use review::{PhotoInspectionRecord, ReviewCursor, ReviewItemRecord, ReviewPageRecord};
pub use store::CatalogStore;
pub use technical_observation::{
    RecordTechnicalObservation, RecordTechnicalObservationStatus, TechnicalObservationRecord,
    TechnicalObservationRevision, TechnicalObservationSummary,
};
pub use writer::{CatalogActor, CatalogHandle};

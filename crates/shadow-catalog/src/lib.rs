//! SQLite-backed catalog persistence.
//!
//! This crate owns the current development schema and write transactions. It deliberately
//! knows nothing about Qt, RAW decoding, or render jobs.
//!
//! Start with [`schema_v1`] for the on-disk shape, [`writer`] for serialized
//! mutation dispatch, and the responsibility-named repository modules below
//! for feature reads and transactions.

mod backup;
mod cache_artifact;
mod decision;
mod decode_snapshot;
mod edit_repository;
mod export_queue;
mod feedback;
mod import_journal;
mod library;
mod library_metadata;
mod recipe;
mod review;
mod schema_v1;
mod store;
mod technical_observation;
mod writer;

use std::{path::Path, time::Duration};

use rusqlite::{Connection, OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{
    AssetLocation, EntityId, LocationId, LocationStatus, PhotoFlag, PhotoId, RepresentationId,
    RepresentationKind,
};
use thiserror::Error;
use uuid::Uuid;

pub use backup::{
    CatalogBackupError, CatalogBackupReceipt, CatalogBackupVerification, create_catalog_backup,
    verify_catalog_backup,
};
pub use cache_artifact::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRecord, CachedArtifactRole,
    InvalidateCachedArtifactStatus, LiveCachedArtifactBlob, RecordCachedArtifact,
    RecordCachedArtifactStatus,
};
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
pub use review::{ReviewCursor, ReviewItemRecord, ReviewPageRecord};
pub use store::CatalogStore;
pub use technical_observation::{
    RecordTechnicalObservation, RecordTechnicalObservationStatus, TechnicalObservationRecord,
    TechnicalObservationRevision, TechnicalObservationSummary,
};
pub use writer::{CatalogActor, CatalogHandle};

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
    #[error("invalid content identity: {0}")]
    InvalidContentIdentity(String),
    #[error("invalid Library metadata facts: {0}")]
    InvalidLibraryFacts(String),
    #[error("invalid Library photo state: {0}")]
    InvalidLibraryState(String),
    #[error("invalid Library album: {0}")]
    InvalidAlbum(String),
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

#[derive(Debug)]
pub struct Catalog {
    connection: Connection,
}

#[derive(Debug, Clone)]
pub struct RegisterAsset {
    pub kind: RepresentationKind,
    pub location: AssetLocation,
    pub byte_len: u64,
    pub modified_at_ms: Option<i64>,
    pub now_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RegisteredAsset {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location_id: LocationId,
    pub status: RegistrationStatus,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RegistrationStatus {
    Inserted,
    Unchanged,
    NeedsRevalidation,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Default)]
pub struct CatalogStats {
    pub photos: u64,
    pub representations: u64,
    pub locations: u64,
    pub locations_needing_revalidation: u64,
}

#[derive(Debug)]
struct ExistingAsset {
    photo_id: PhotoId,
    representation_id: RepresentationId,
    location_id: LocationId,
    byte_len: u64,
    modified_at_ms: Option<i64>,
}

impl Catalog {
    /// Opens or creates a file-backed catalog using the one current development
    /// schema. A catalog from an earlier development shape is rejected rather
    /// than migrated.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot open, configure, or initialize
    /// the catalog.
    pub fn open(path: &Path) -> Result<Self, CatalogError> {
        let mut connection = Connection::open(path)?;
        configure_connection(&connection, true)?;
        schema_v1::initialize(&mut connection)?;
        Ok(Self { connection })
    }

    /// Opens an isolated in-memory catalog, primarily for tests.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot initialize the
    /// in-memory database.
    pub fn open_in_memory() -> Result<Self, CatalogError> {
        let mut connection = Connection::open_in_memory()?;
        configure_connection(&connection, false)?;
        schema_v1::initialize(&mut connection)?;
        Ok(Self { connection })
    }

    /// Returns the active catalog schema version.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the schema state cannot be queried.
    pub fn schema_version(&self) -> Result<i64, CatalogError> {
        schema_v1::current_version(&self.connection).map_err(Into::into)
    }

    /// Registers an asset path atomically and idempotently.
    ///
    /// A path whose size or modification time changed is marked for later
    /// content revalidation instead of silently replacing its representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the registration transaction cannot be
    /// queried, written, or committed.
    pub fn register_asset(
        &mut self,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        let transaction = self.connection.transaction()?;
        let result = register_asset_in_transaction(&transaction, request)?;
        transaction.commit()?;
        Ok(result)
    }

    /// Returns persisted entity counts and the revalidation backlog.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if any count query fails or returns an invalid
    /// negative value.
    pub fn stats(&self) -> Result<CatalogStats, CatalogError> {
        let locations_needing_revalidation = self.connection.query_row(
            "SELECT COUNT(*) FROM locations WHERE status = ?1",
            [LocationStatus::NeedsRevalidation.as_str()],
            |row| row.get(0),
        )?;

        Ok(CatalogStats {
            photos: count_rows(&self.connection, "photos")?,
            representations: count_rows(&self.connection, "representations")?,
            locations: count_rows(&self.connection, "locations")?,
            locations_needing_revalidation: non_negative_count(locations_needing_revalidation)?,
        })
    }
}

fn register_asset_in_transaction(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
) -> rusqlite::Result<RegisteredAsset> {
    let existing = find_existing_asset(transaction, &request.location)?;

    if let Some(existing) = existing {
        let unchanged = existing.byte_len == request.byte_len
            && existing.modified_at_ms == request.modified_at_ms;

        if !unchanged {
            let byte_len = i64::try_from(request.byte_len)
                .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;
            // A location may be overwritten in place. Its representation
            // keeps the logical photo identity, but every byte-derived proof
            // belongs to the old revision and must be discarded atomically.
            // Updating the representation fingerprint also naturally
            // invalidates decoder snapshots and cache artifacts keyed by it.
            transaction.execute(
                "UPDATE representations SET byte_len = ?2, modified_at_ms = ?3 WHERE id = ?1",
                params![
                    existing.representation_id.as_bytes().as_slice(),
                    byte_len,
                    request.modified_at_ms,
                ],
            )?;
            transaction.execute(
                "DELETE FROM representation_content_identities WHERE representation_id = ?1",
                [existing.representation_id.as_bytes().as_slice()],
            )?;
            transaction.execute(
                "UPDATE locations SET status = ?1 WHERE id = ?2",
                params![
                    LocationStatus::NeedsRevalidation.as_str(),
                    existing.location_id.as_bytes().as_slice()
                ],
            )?;
        }

        Ok(RegisteredAsset {
            photo_id: existing.photo_id,
            representation_id: existing.representation_id,
            location_id: existing.location_id,
            status: if unchanged {
                RegistrationStatus::Unchanged
            } else {
                RegistrationStatus::NeedsRevalidation
            },
        })
    } else {
        insert_asset(transaction, request)
    }
}

fn configure_connection(connection: &Connection, file_backed: bool) -> rusqlite::Result<()> {
    connection.busy_timeout(Duration::from_secs(5))?;
    connection.execute_batch(
        "PRAGMA foreign_keys = ON;
         PRAGMA temp_store = MEMORY;",
    )?;

    if file_backed {
        connection.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;",
        )?;
    }

    Ok(())
}

fn find_existing_asset(
    transaction: &Transaction<'_>,
    location: &AssetLocation,
) -> rusqlite::Result<Option<ExistingAsset>> {
    transaction
        .query_row(
            "SELECT p.id, r.id, l.id, r.byte_len, r.modified_at_ms
             FROM locations l
             JOIN representations r ON r.id = l.representation_id
             JOIN photos p ON p.id = r.photo_id
             WHERE l.platform = ?1 AND l.native_path = ?2",
            params![location.platform.as_str(), location.native_path],
            |row| {
                let byte_len: i64 = row.get(3)?;
                Ok(ExistingAsset {
                    photo_id: read_id(row, 0)?,
                    representation_id: read_id(row, 1)?,
                    location_id: read_id(row, 2)?,
                    byte_len: u64::try_from(byte_len).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(3, Type::Integer, Box::new(error))
                    })?,
                    modified_at_ms: row.get(4)?,
                })
            },
        )
        .optional()
}

fn insert_asset(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
) -> rusqlite::Result<RegisteredAsset> {
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let location_id = LocationId::new_v7();
    let byte_len = i64::try_from(request.byte_len)
        .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;

    transaction.execute(
        "INSERT INTO photos(id, created_at_ms) VALUES (?1, ?2)",
        params![photo_id.as_bytes().as_slice(), request.now_ms],
    )?;
    transaction.execute(
        "INSERT INTO representations(
             id, photo_id, kind, byte_len, modified_at_ms, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            representation_id.as_bytes().as_slice(),
            photo_id.as_bytes().as_slice(),
            request.kind.as_str(),
            byte_len,
            request.modified_at_ms,
            request.now_ms
        ],
    )?;
    transaction.execute(
        "INSERT INTO locations(
             id, representation_id, platform, native_path, display_path, status, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![
            location_id.as_bytes().as_slice(),
            representation_id.as_bytes().as_slice(),
            request.location.platform.as_str(),
            request.location.native_path,
            request.location.display_path,
            LocationStatus::Online.as_str(),
            request.now_ms
        ],
    )?;

    Ok(RegisteredAsset {
        photo_id,
        representation_id,
        location_id,
        status: RegistrationStatus::Inserted,
    })
}

fn read_id<I: EntityId>(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<I> {
    let bytes: Vec<u8> = row.get(index)?;
    let uuid = Uuid::from_slice(&bytes).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
    })?;
    Ok(I::from_uuid(uuid))
}

fn count_rows(connection: &Connection, table: &str) -> rusqlite::Result<u64> {
    let sql = match table {
        "photos" => "SELECT COUNT(*) FROM photos",
        "representations" => "SELECT COUNT(*) FROM representations",
        "locations" => "SELECT COUNT(*) FROM locations",
        _ => return Err(rusqlite::Error::InvalidQuery),
    };
    let count = connection.query_row(sql, [], |row| row.get(0))?;
    non_negative_count(count)
}

fn non_negative_count(count: i64) -> rusqlite::Result<u64> {
    u64::try_from(count).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(0, Type::Integer, Box::new(error))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use shadow_domain::Platform;

    fn request(byte_len: u64, modified_at_ms: Option<i64>) -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/DSC_0001.NEF".to_vec(),
                "/photos/DSC_0001.NEF",
            ),
            byte_len,
            modified_at_ms,
            now_ms: 1_700_000_000_000,
        }
    }

    #[test]
    fn registering_the_same_location_is_idempotent() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let first = catalog
            .register_asset(&request(42, Some(100)))
            .expect("first registration");
        let second = catalog
            .register_asset(&request(42, Some(100)))
            .expect("second registration");

        assert_eq!(first.status, RegistrationStatus::Inserted);
        assert_eq!(second.status, RegistrationStatus::Unchanged);
        assert_eq!(first.photo_id, second.photo_id);
        assert_eq!(catalog.stats().expect("stats").photos, 1);
    }

    #[test]
    fn changed_file_is_marked_for_revalidation_without_silent_replacement() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        catalog
            .register_asset(&request(42, Some(100)))
            .expect("initial registration");
        let changed = catalog
            .register_asset(&request(84, Some(200)))
            .expect("changed registration");

        assert_eq!(changed.status, RegistrationStatus::NeedsRevalidation);
        assert_eq!(
            catalog
                .stats()
                .expect("stats")
                .locations_needing_revalidation,
            1
        );
    }
}

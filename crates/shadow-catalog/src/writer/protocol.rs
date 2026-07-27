//! Internal request protocol between cloned Catalog handles and the single
//! connection-owning actor thread.
//!
//! Variants are grouped by feature vocabulary, while dispatch and client
//! methods remain separate consumers of this contract.

use super::*;

mod edit_history;
mod evidence;
mod export_preset;
mod export_queue;
mod import_journal;
mod library_browse;
mod library_collections;
mod library_facts;
mod source_health;
mod source_identity;
pub(super) use edit_history::EditHistoryMessage;
pub(super) use evidence::{DecisionMessage, FeedbackMessage};
pub(super) use export_preset::ExportPresetMessage;
pub(super) use export_queue::ExportQueueMessage;
pub(super) use import_journal::ImportJournalMessage;
pub(super) use library_browse::LibraryBrowseMessage;
pub(super) use library_collections::LibraryCollectionsMessage;
pub(super) use library_facts::LibraryFactsMessage;
pub(super) use source_health::SourceHealthMessage;
pub(super) use source_identity::SourceIdentityMessage;

pub(super) enum Message {
    SchemaVersion(SyncSender<Result<i64, CatalogError>>),
    Stats(SyncSender<Result<CatalogStats, CatalogError>>),
    SourceIdentity(SourceIdentityMessage),
    SourceHealth(SourceHealthMessage),
    LibraryFacts(LibraryFactsMessage),
    LibraryCollections(LibraryCollectionsMessage),
    LibraryBrowse(LibraryBrowseMessage),
    RecordDecodeSnapshot(
        Box<RecordDecodeSnapshot>,
        SyncSender<Result<RecordDecodeSnapshotStatus, CatalogError>>,
    ),
    DecodeSnapshots(
        RepresentationId,
        SyncSender<Result<Vec<DecodeSnapshotRecord>, CatalogError>>,
    ),
    IsDecodeOutputCurrent(
        RepresentationId,
        String,
        String,
        RepresentationFingerprint,
        bool,
        String,
        Option<String>,
        SyncSender<Result<bool, CatalogError>>,
    ),
    RecordCachedArtifact(
        Box<RecordCachedArtifact>,
        SyncSender<Result<RecordCachedArtifactStatus, CatalogError>>,
    ),
    CachedArtifacts(
        RepresentationId,
        SyncSender<Result<Vec<CachedArtifactRecord>, CatalogError>>,
    ),
    PreferredCachedArtifact(
        RepresentationId,
        SyncSender<Result<Option<CachedArtifactRecord>, CatalogError>>,
    ),
    PreferredCachedArtifacts(
        Vec<RepresentationId>,
        SyncSender<Result<Vec<Option<CachedArtifactRecord>>, CatalogError>>,
    ),
    LiveCachedArtifactBlobs(SyncSender<Result<Vec<LiveCachedArtifactBlob>, CatalogError>>),
    InvalidateCachedArtifact(
        Box<CachedArtifactRecord>,
        SyncSender<Result<InvalidateCachedArtifactStatus, CatalogError>>,
    ),
    RecordTechnicalObservation(
        Box<RecordTechnicalObservation>,
        SyncSender<Result<RecordTechnicalObservationStatus, CatalogError>>,
    ),
    TechnicalObservation(
        RepresentationId,
        RepresentationFingerprint,
        Box<crate::CachedArtifact>,
        TechnicalObservationRevision,
        SyncSender<Result<Option<TechnicalObservationRecord>, CatalogError>>,
    ),
    ReviewPage(
        Option<ReviewCursor>,
        usize,
        Option<TechnicalObservationRevision>,
        Option<CachedArtifactGeneratorIdentity>,
        SyncSender<Result<ReviewPageRecord, CatalogError>>,
    ),
    ReviewSource(
        PhotoId,
        Option<TechnicalObservationRevision>,
        SyncSender<Result<Option<ReviewItemRecord>, CatalogError>>,
    ),
    PhotoSource(
        PhotoId,
        SyncSender<Result<Option<ReviewItemRecord>, CatalogError>>,
    ),
    EditHistory(EditHistoryMessage),
    ExportPreset(ExportPresetMessage),
    ExportQueue(ExportQueueMessage),
    Decision(DecisionMessage),
    Feedback(FeedbackMessage),
    ImportJournal(ImportJournalMessage),
    Shutdown(SyncSender<()>),
}

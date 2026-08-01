//! Internal request protocol between cloned Catalog handles and the single
//! connection-owning actor thread.
//!
//! Variants are grouped by feature vocabulary, while dispatch and client
//! methods remain separate consumers of this contract.

use super::{CatalogError, CatalogStats, SyncSender};

mod cached_artifact;
mod decode_snapshot;
mod edit_history;
mod evidence;
mod export_preset;
mod export_queue;
mod import_journal;
mod library_browse;
mod library_collections;
mod library_facts;
mod library_keywords;
mod library_lifecycle;
mod library_metadata_overrides;
mod library_place_resolution;
mod review_projection;
mod source_health;
mod source_identity;
mod technical_observation;
pub(super) use cached_artifact::CachedArtifactMessage;
pub(super) use decode_snapshot::DecodeSnapshotMessage;
pub(super) use edit_history::EditHistoryMessage;
pub(super) use evidence::{DecisionMessage, FeedbackMessage};
pub(super) use export_preset::ExportPresetMessage;
pub(super) use export_queue::ExportQueueMessage;
pub(super) use import_journal::ImportJournalMessage;
pub(super) use library_browse::LibraryBrowseMessage;
pub(super) use library_collections::LibraryCollectionsMessage;
pub(super) use library_facts::LibraryFactsMessage;
pub(super) use library_keywords::LibraryKeywordsMessage;
pub(super) use library_lifecycle::LibraryLifecycleMessage;
pub(super) use library_metadata_overrides::LibraryMetadataOverridesMessage;
pub(super) use library_place_resolution::LibraryPlaceResolutionMessage;
pub(super) use review_projection::ReviewProjectionMessage;
pub(super) use source_health::SourceHealthMessage;
pub(super) use source_identity::SourceIdentityMessage;
pub(super) use technical_observation::TechnicalObservationMessage;

pub(super) enum Message {
    SchemaVersion(SyncSender<Result<i64, CatalogError>>),
    Stats(SyncSender<Result<CatalogStats, CatalogError>>),
    SourceIdentity(SourceIdentityMessage),
    SourceHealth(SourceHealthMessage),
    LibraryFacts(LibraryFactsMessage),
    LibraryKeywords(LibraryKeywordsMessage),
    LibraryLifecycle(LibraryLifecycleMessage),
    LibraryMetadataOverrides(LibraryMetadataOverridesMessage),
    LibraryCollections(LibraryCollectionsMessage),
    LibraryBrowse(LibraryBrowseMessage),
    LibraryPlaceResolution(LibraryPlaceResolutionMessage),
    DecodeSnapshot(DecodeSnapshotMessage),
    CachedArtifact(CachedArtifactMessage),
    TechnicalObservation(TechnicalObservationMessage),
    ReviewProjection(ReviewProjectionMessage),
    EditHistory(EditHistoryMessage),
    ExportPreset(ExportPresetMessage),
    ExportQueue(ExportQueueMessage),
    Decision(DecisionMessage),
    Feedback(FeedbackMessage),
    ImportJournal(ImportJournalMessage),
    Shutdown(SyncSender<()>),
}

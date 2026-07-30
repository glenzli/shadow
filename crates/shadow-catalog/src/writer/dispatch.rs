//! Actor-side execution of the internal writer protocol.
//!
//! This module is deliberately a thin mapping from each message variant to
//! the responsibility-named synchronous `Catalog` operation.

use super::{Catalog, CatalogError, Message, Receiver, SyncSender};

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
mod library_metadata_overrides;
mod review_projection;
mod source_health;
mod source_identity;
mod technical_observation;
use cached_artifact::run_cached_artifact_message;
use decode_snapshot::run_decode_snapshot_message;
use edit_history::run_edit_history_message;
use evidence::{run_decision_message, run_feedback_message};
use export_preset::run_export_preset_message;
use export_queue::run_export_queue_message;
use import_journal::run_import_journal_message;
use library_browse::run_library_browse_message;
use library_collections::run_library_collections_message;
use library_facts::run_library_facts_message;
use library_keywords::run_library_keywords_message;
use library_metadata_overrides::run_library_metadata_overrides_message;
use review_projection::run_review_projection_message;
use source_health::run_source_health_message;
use source_identity::run_source_identity_message;
use technical_observation::run_technical_observation_message;

pub(super) fn run_actor(mut catalog: Catalog, receiver: &Receiver<Message>) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::SchemaVersion(response) => respond(&response, catalog.schema_version()),
            Message::Stats(response) => respond(&response, catalog.stats()),
            Message::SourceIdentity(message) => run_source_identity_message(&mut catalog, message),
            Message::SourceHealth(message) => run_source_health_message(&mut catalog, message),
            Message::LibraryFacts(message) => run_library_facts_message(&mut catalog, message),
            Message::LibraryKeywords(message) => {
                run_library_keywords_message(&mut catalog, message);
            }
            Message::LibraryMetadataOverrides(message) => {
                run_library_metadata_overrides_message(&mut catalog, message);
            }
            Message::LibraryCollections(message) => {
                run_library_collections_message(&mut catalog, message);
            }
            Message::LibraryBrowse(message) => run_library_browse_message(&mut catalog, message),
            Message::DecodeSnapshot(message) => run_decode_snapshot_message(&mut catalog, message),
            Message::CachedArtifact(message) => run_cached_artifact_message(&mut catalog, message),
            Message::TechnicalObservation(message) => {
                run_technical_observation_message(&mut catalog, message);
            }
            Message::ReviewProjection(message) => {
                run_review_projection_message(&mut catalog, message);
            }
            Message::EditHistory(message) => run_edit_history_message(&mut catalog, message),
            Message::ExportPreset(message) => run_export_preset_message(&mut catalog, message),
            Message::ExportQueue(message) => run_export_queue_message(&mut catalog, message),
            Message::Decision(message) => run_decision_message(&mut catalog, message),
            Message::Feedback(message) => run_feedback_message(&mut catalog, message),
            Message::ImportJournal(message) => run_import_journal_message(&mut catalog, message),
            Message::Shutdown(response) => {
                let _ = response.send(());
                break;
            }
        }
    }
}

fn respond<T>(sender: &SyncSender<Result<T, CatalogError>>, result: Result<T, CatalogError>) {
    let _ = sender.send(result);
}

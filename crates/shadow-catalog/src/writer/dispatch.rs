//! Actor-side execution of the internal writer protocol.
//!
//! This module is deliberately a thin mapping from each message variant to
//! the responsibility-named synchronous `Catalog` operation.

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
use edit_history::run_edit_history_message;
use evidence::{run_decision_message, run_feedback_message};
use export_preset::run_export_preset_message;
use export_queue::run_export_queue_message;
use import_journal::run_import_journal_message;
use library_browse::run_library_browse_message;
use library_collections::run_library_collections_message;
use library_facts::run_library_facts_message;
use source_health::run_source_health_message;
use source_identity::run_source_identity_message;

#[allow(clippy::too_many_lines)]
pub(super) fn run_actor(mut catalog: Catalog, receiver: &Receiver<Message>) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::SchemaVersion(response) => respond(&response, catalog.schema_version()),
            Message::Stats(response) => respond(&response, catalog.stats()),
            Message::SourceIdentity(message) => run_source_identity_message(&mut catalog, message),
            Message::SourceHealth(message) => run_source_health_message(&mut catalog, message),
            Message::LibraryFacts(message) => run_library_facts_message(&mut catalog, message),
            Message::LibraryCollections(message) => {
                run_library_collections_message(&mut catalog, message);
            }
            Message::LibraryBrowse(message) => run_library_browse_message(&mut catalog, message),
            Message::RecordDecodeSnapshot(request, response) => {
                let _ = response.send(catalog.record_decode_snapshot(request.as_ref()));
            }
            Message::DecodeSnapshots(representation_id, response) => {
                let _ = response.send(catalog.decode_snapshots(representation_id));
            }
            Message::IsDecodeOutputCurrent(
                representation_id,
                provider_id,
                provider_version,
                source,
                require_cached_preview,
                proxy_variant_key,
                required_technical_preprocessing,
                response,
            ) => {
                let _ = response.send(catalog.is_decode_output_current(
                    representation_id,
                    &provider_id,
                    &provider_version,
                    source,
                    require_cached_preview,
                    &proxy_variant_key,
                    required_technical_preprocessing.as_deref(),
                ));
            }
            Message::RecordCachedArtifact(request, response) => {
                let _ = response.send(catalog.record_cached_artifact(request.as_ref()));
            }
            Message::CachedArtifacts(representation_id, response) => {
                let _ = response.send(catalog.cached_artifacts(representation_id));
            }
            Message::PreferredCachedArtifact(representation_id, response) => {
                let _ = response.send(catalog.preferred_cached_artifact(representation_id));
            }
            Message::PreferredCachedArtifacts(representation_ids, response) => {
                let _ = response.send(catalog.preferred_cached_artifacts(&representation_ids));
            }
            Message::LiveCachedArtifactBlobs(response) => {
                let _ = response.send(catalog.live_cached_artifact_blobs());
            }
            Message::InvalidateCachedArtifact(record, response) => {
                let _ = response.send(catalog.invalidate_cached_artifact(record.as_ref()));
            }
            Message::RecordTechnicalObservation(request, response) => {
                let _ = response.send(catalog.record_technical_observation(request.as_ref()));
            }
            Message::TechnicalObservation(
                representation_id,
                source,
                artifact,
                revision,
                response,
            ) => {
                let _ = response.send(catalog.technical_observation(
                    representation_id,
                    source,
                    artifact.as_ref(),
                    &revision,
                ));
            }
            Message::ReviewPage(after, limit, revision, recipe_preview_generator, response) => {
                let result = match (revision.as_ref(), recipe_preview_generator.as_ref()) {
                    (Some(revision), Some(recipe_preview_generator)) => catalog
                        .review_page_with_technical_and_recipe_preview_generator(
                            after.as_ref(),
                            limit,
                            revision,
                            recipe_preview_generator,
                        ),
                    (Some(revision), None) => {
                        catalog.review_page_with_technical(after.as_ref(), limit, revision)
                    }
                    (None, None) => catalog.review_page(after.as_ref(), limit),
                    (None, Some(_)) => unreachable!(
                        "a Recipe-preview generator filter requires a technical Review query"
                    ),
                };
                let _ = response.send(result);
            }
            Message::ReviewSource(photo_id, revision, response) => {
                let result = revision.as_ref().map_or_else(
                    || catalog.review_source(photo_id),
                    |revision| catalog.review_source_with_technical(photo_id, revision),
                );
                let _ = response.send(result);
            }
            Message::PhotoSource(photo_id, response) => {
                let _ = response.send(catalog.photo_source(photo_id));
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

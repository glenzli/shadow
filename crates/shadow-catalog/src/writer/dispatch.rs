//! Actor-side execution of the internal writer protocol.
//!
//! This module is deliberately a thin mapping from each message variant to
//! the responsibility-named synchronous `Catalog` operation.

use super::*;

mod edit_history;
mod evidence;
use edit_history::run_edit_history_message;
use evidence::{run_decision_message, run_feedback_message};

#[allow(clippy::too_many_lines)]
pub(super) fn run_actor(mut catalog: Catalog, receiver: &Receiver<Message>) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::SchemaVersion(response) => respond(&response, catalog.schema_version()),
            Message::Stats(response) => respond(&response, catalog.stats()),
            Message::RegisterAsset(request, response) => {
                let _ = response.send(catalog.register_asset(&request));
            }
            Message::RegisterAssetWithContentIdentity(request, identity, response) => {
                let _ = response
                    .send(catalog.register_asset_with_content_identity(&request, &identity));
            }
            Message::RecordRepresentationContentIdentity(request, response) => {
                let _ = response.send(catalog.record_representation_content_identity(&request));
            }
            Message::RelinkMatch(identity, response) => {
                let _ = response.send(catalog.relink_match(&identity));
            }
            Message::MissingSourceRelinkTarget(scan_session_id, location_id, response) => {
                let _ = response
                    .send(catalog.missing_source_relink_target(scan_session_id, location_id));
            }
            Message::UpsertPhotoLibraryFacts(facts, response) => {
                let _ = response.send(catalog.upsert_photo_library_facts(facts.as_ref()));
            }
            Message::PhotoLibraryFacts(photo_id, response) => {
                let _ = response.send(catalog.photo_library_facts(photo_id));
            }
            Message::SetPhotoLibraryState(state, response) => {
                let _ = response.send(catalog.set_photo_library_state(state.as_ref()));
            }
            Message::PhotoLibraryState(photo_id, response) => {
                let _ = response.send(catalog.photo_library_state(photo_id));
            }
            Message::CreateLibraryAlbum(kind, name, query_json, now_ms, response) => {
                let _ = response.send(catalog.create_library_album(
                    kind,
                    &name,
                    query_json.as_deref(),
                    now_ms,
                ));
            }
            Message::CreateSmartLibraryAlbum(name, query, now_ms, response) => {
                let _ = response.send(catalog.create_smart_library_album(&name, &query, now_ms));
            }
            Message::RenameLibraryAlbum(album_id, name, now_ms, response) => {
                let _ = response.send(catalog.rename_library_album(album_id, &name, now_ms));
            }
            Message::ReplaceSmartAlbumQuery(album_id, query, now_ms, response) => {
                let _ = response.send(catalog.replace_smart_album_query(album_id, &query, now_ms));
            }
            Message::DeleteLibraryAlbum(album_id, response) => {
                let _ = response.send(catalog.delete_library_album(album_id));
            }
            Message::LibraryAlbums(response) => {
                let _ = response.send(catalog.library_albums());
            }
            Message::AddPhotoToAlbum(album_id, photo_id, sort_key, now_ms, response) => {
                let _ =
                    response.send(catalog.add_photo_to_album(album_id, photo_id, sort_key, now_ms));
            }
            Message::RemovePhotoFromAlbum(album_id, photo_id, response) => {
                let _ = response.send(catalog.remove_photo_from_album(album_id, photo_id));
            }
            Message::AlbumsForPhoto(photo_id, response) => {
                let _ = response.send(catalog.albums_for_photo(photo_id));
            }
            Message::LibrarySources(response) => {
                let _ = response.send(catalog.library_sources());
            }
            Message::LibrarySourceHealth(response) => {
                let _ = response.send(catalog.library_source_health());
            }
            Message::MissingSourceLocationPage(
                scan_session_id,
                after,
                requested_limit,
                response,
            ) => {
                let _ = response.send(catalog.missing_source_location_page(
                    scan_session_id,
                    after.as_ref(),
                    requested_limit,
                ));
            }
            Message::LibraryPhotoPage(filter, after, requested_limit, response) => {
                let _ = response.send(catalog.library_photo_page(
                    &filter,
                    after.as_ref(),
                    requested_limit,
                ));
            }
            Message::LibraryFacetPage(filter, kind, after, requested_limit, response) => {
                let _ = response.send(catalog.library_facet_page(
                    &filter,
                    kind,
                    after.as_ref(),
                    requested_limit,
                ));
            }
            Message::LibraryPhotoCount(filter, response) => {
                let _ = response.send(catalog.library_photo_count(&filter));
            }
            Message::SmartAlbumFilter(album_id, response) => {
                let _ = response.send(catalog.smart_album_filter(album_id));
            }
            Message::SmartAlbumPhotoPage(album_id, after, requested_limit, response) => {
                let _ = response.send(catalog.smart_album_photo_page(
                    album_id,
                    after.as_ref(),
                    requested_limit,
                ));
            }
            Message::SmartAlbumPhotoCount(album_id, response) => {
                let _ = response.send(catalog.smart_album_photo_count(album_id));
            }
            Message::RepresentationFingerprint(representation_id, response) => {
                let _ = response.send(catalog.representation_fingerprint(representation_id));
            }
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
            Message::CreateExportPreset(name, settings_json, now_ms, response) => {
                let _ = response.send(catalog.create_export_preset(&name, &settings_json, now_ms));
            }
            Message::ReviseExportPreset(preset_id, settings_json, now_ms, response) => {
                let _ =
                    response.send(catalog.revise_export_preset(preset_id, &settings_json, now_ms));
            }
            Message::ExportPresets(response) => {
                let _ = response.send(catalog.export_presets());
            }
            Message::ExportPresetRevisions(preset_id, response) => {
                let _ = response.send(catalog.export_preset_revisions(preset_id));
            }
            Message::EnqueueExportJob(request, response) => {
                let _ = response.send(catalog.enqueue_export_job(request.as_ref()));
            }
            Message::ExportJob(job_id, response) => {
                respond(&response, catalog.export_job(job_id));
            }
            Message::ExportJobs(limit, response) => {
                let _ = response.send(catalog.export_jobs(limit));
            }
            Message::ExportJobItems(job_id, response) => {
                let _ = response.send(catalog.export_job_items(job_id));
            }
            Message::ExportItem(item_id, response) => {
                respond(&response, catalog.export_item(item_id));
            }
            Message::ExportJobProgress(job_id, response) => {
                respond(&response, catalog.export_job_progress(job_id));
            }
            Message::ClaimNextExportItem(now_ms, response) => {
                let _ = response.send(catalog.claim_next_export_item(now_ms));
            }
            Message::AdvanceExportItem(request, response) => {
                let _ = response.send(catalog.advance_export_item(request.as_ref()));
            }
            Message::ExportOutputReceipt(item_id, response) => {
                respond(&response, catalog.export_output_receipt(item_id));
            }
            Message::CancelExportJob(job_id, now_ms, response) => {
                let _ = response.send(catalog.cancel_export_job(job_id, now_ms));
            }
            Message::RecoverInterruptedExportJobs(now_ms, response) => {
                let _ = response.send(catalog.recover_interrupted_export_jobs(now_ms));
            }
            Message::RecoverAndRequeueInterruptedExportItems(now_ms, response) => {
                let _ = response.send(catalog.recover_and_requeue_interrupted_export_items(now_ms));
            }
            Message::Decision(message) => run_decision_message(&mut catalog, message),
            Message::Feedback(message) => run_feedback_message(&mut catalog, message),
            Message::BeginImportSession(root, now_ms, response) => {
                let _ = response.send(catalog.begin_import_session(&root, now_ms));
            }
            Message::BeginRelocationSession(root, now_ms, response) => {
                let _ = response.send(catalog.begin_relocation_session(&root, now_ms));
            }
            Message::ResumeImportSession(id, now_ms, response) => {
                let _ = response.send(catalog.resume_import_session(id, now_ms));
            }
            Message::RecordImportDiscovered(id, request, response) => {
                let _ = response.send(catalog.record_import_discovered(id, &request));
            }
            Message::RegisterImportAsset(id, request, response) => {
                let _ = response.send(catalog.register_import_asset(id, &request));
            }
            Message::RegisterImportVerifiedRelocation(
                id,
                request,
                expected_representation_id,
                identity,
                response,
            ) => {
                let _ = response.send(catalog.register_import_verified_relocation(
                    id,
                    &request,
                    expected_representation_id,
                    &identity,
                ));
            }
            Message::RecordImportIssue(id, location, message, now_ms, response) => {
                let _ = response.send(catalog.record_import_issue(id, &location, &message, now_ms));
            }
            Message::FinishImportSession(id, state, error, now_ms, response) => {
                let _ = response.send(catalog.finish_import_session(
                    id,
                    state,
                    error.as_deref(),
                    now_ms,
                ));
            }
            Message::ImportSessionSummary(id, response) => {
                let _ = response.send(catalog.import_session_summary(id));
            }
            Message::UnfinishedImportSessions(response) => {
                let _ = response.send(catalog.unfinished_import_sessions());
            }
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

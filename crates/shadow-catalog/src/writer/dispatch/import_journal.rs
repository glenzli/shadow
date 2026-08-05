//! Actor-side execution for durable import-journal messages.

use crate::Catalog;

use super::super::protocol::ImportJournalMessage;

pub(super) fn run_import_journal_message(catalog: &mut Catalog, message: ImportJournalMessage) {
    match message {
        ImportJournalMessage::BeginImportSession(root, now_ms, response) => {
            let _ = response.send(catalog.begin_import_session(&root, now_ms));
        }
        ImportJournalMessage::BeginRelocationSession(root, now_ms, response) => {
            let _ = response.send(catalog.begin_relocation_session(&root, now_ms));
        }
        ImportJournalMessage::ResumeImportSession(id, now_ms, response) => {
            let _ = response.send(catalog.resume_import_session(id, now_ms));
        }
        ImportJournalMessage::RecordImportDiscovered(id, request, response) => {
            let _ = response.send(catalog.record_import_discovered(id, &request));
        }
        ImportJournalMessage::RegisterImportAsset(id, request, response) => {
            let _ = response.send(catalog.register_import_asset(id, &request));
        }
        ImportJournalMessage::RegisterImportAssetGrouped(id, request, grouping, response) => {
            let _ = response.send(catalog.register_import_asset_grouped(id, &request, &grouping));
        }
        ImportJournalMessage::RegisterImportVerifiedRelocation(
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
        ImportJournalMessage::RecordImportIssue(id, location, message, now_ms, response) => {
            let _ = response.send(catalog.record_import_issue(id, &location, &message, now_ms));
        }
        ImportJournalMessage::FinishImportSession(id, state, error, now_ms, response) => {
            let _ =
                response.send(catalog.finish_import_session(id, state, error.as_deref(), now_ms));
        }
        ImportJournalMessage::ImportSessionSummary(id, response) => {
            let _ = response.send(catalog.import_session_summary(id));
        }
        ImportJournalMessage::UnfinishedImportSessions(response) => {
            let _ = response.send(catalog.unfinished_import_sessions());
        }
    }
}

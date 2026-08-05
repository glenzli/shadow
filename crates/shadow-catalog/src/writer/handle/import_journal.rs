//! Client adapters and scanner-store bridge for durable import journals.

use shadow_domain::{AssetLocation, ImportSessionId, RepresentationId};

use crate::{
    CatalogError, CatalogStore, ContentIdentity, ImportPhotoGrouping, ImportSession,
    ImportSessionState, ImportSessionSummary, RegisterAsset, RegisteredAsset,
};

use super::super::{
    CatalogHandle,
    protocol::{ImportJournalMessage, Message},
};

impl CatalogHandle {
    /// Atomically binds a freshly discovered location to an already verified
    /// representation identity and records the import-journal result through
    /// the single catalog writer.
    ///
    /// This is intentionally an explicit relocation operation: a normal scan
    /// may never infer a merge from names, timestamps, or file sizes alone.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the discovered entry is absent, the target
    /// path already exists, or the exact identity does not still belong to the
    /// declared representation.
    pub fn register_import_verified_relocation(
        &self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::RegisterImportVerifiedRelocation(
                session_id,
                request.clone(),
                expected_representation_id,
                identity.clone(),
                response,
            ))
        })
    }

    /// Starts an explicit relocation journal which intentionally has no
    /// Library discovery source.
    pub fn begin_relocation_session(
        &self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::BeginRelocationSession(
                root.clone(),
                now_ms,
                response,
            ))
        })
    }

    /// Lists resumable import sessions.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn unfinished_import_sessions(&self) -> Result<Vec<ImportSession>, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::UnfinishedImportSessions(response))
        })
    }

    /// Returns the durable summary for one import session.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the session is
    /// absent, or the query fails.
    pub fn import_session_summary(
        &self,
        id: ImportSessionId,
    ) -> Result<ImportSessionSummary, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::ImportSessionSummary(id, response))
        })
    }
}

impl CatalogStore for CatalogHandle {
    fn begin_import_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::BeginImportSession(
                root.clone(),
                now_ms,
                response,
            ))
        })
    }

    fn resume_import_session(
        &mut self,
        id: ImportSessionId,
        now_ms: i64,
    ) -> Result<ImportSession, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::ResumeImportSession(
                id, now_ms, response,
            ))
        })
    }

    fn record_import_discovered(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::RecordImportDiscovered(
                session_id,
                request.clone(),
                response,
            ))
        })
    }

    fn register_import_asset(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::RegisterImportAsset(
                session_id,
                request.clone(),
                response,
            ))
        })
    }

    fn register_import_asset_grouped(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        grouping: &ImportPhotoGrouping,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::RegisterImportAssetGrouped(
                session_id,
                request.clone(),
                grouping.clone(),
                response,
            ))
        })
    }

    fn register_import_verified_relocation(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        Self::register_import_verified_relocation(
            self,
            session_id,
            request,
            expected_representation_id,
            identity,
        )
    }

    fn record_import_issue(
        &mut self,
        session_id: ImportSessionId,
        location: &AssetLocation,
        message: &str,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::RecordImportIssue(
                session_id,
                location.clone(),
                message.to_owned(),
                now_ms,
                response,
            ))
        })
    }

    fn finish_import_session(
        &mut self,
        id: ImportSessionId,
        state: ImportSessionState,
        last_error: Option<&str>,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::ImportJournal(ImportJournalMessage::FinishImportSession(
                id,
                state,
                last_error.map(str::to_owned),
                now_ms,
                response,
            ))
        })
    }
}

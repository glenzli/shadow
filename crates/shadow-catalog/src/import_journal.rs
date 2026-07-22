use rusqlite::{OptionalExtension, params, types::Type};
use shadow_domain::{AssetLocation, EntityId, ImportSessionId, LibrarySourceId, Platform};
use uuid::Uuid;

use crate::library::{attach_location_to_library_source, upsert_library_source_in_transaction};
use crate::{
    Catalog, CatalogError, RegisterAsset, RegisteredAsset, RegistrationStatus, non_negative_count,
    read_id, register_asset_in_transaction,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImportSessionState {
    Running,
    Completed,
    Failed,
    Cancelled,
}

impl ImportSessionState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Running => "running",
            Self::Completed => "completed",
            Self::Failed => "failed",
            Self::Cancelled => "cancelled",
        }
    }

    const fn is_terminal(self) -> bool {
        matches!(self, Self::Completed | Self::Failed | Self::Cancelled)
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImportSession {
    pub id: ImportSessionId,
    /// The durable discovery source backing this session. It is intentionally
    /// optional for catalogs migrated from before the Library source model.
    pub source_id: Option<LibrarySourceId>,
    pub root: AssetLocation,
    pub state: ImportSessionState,
    pub started_at_ms: i64,
    pub updated_at_ms: i64,
    pub finished_at_ms: Option<i64>,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImportSessionSummary {
    pub session: ImportSession,
    pub discovered: u64,
    pub inserted: u64,
    pub unchanged: u64,
    pub needs_revalidation: u64,
    pub failed_entries: u64,
    pub issues: u64,
}

impl Catalog {
    /// Starts a durable import journal for a root folder.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the session cannot be inserted.
    pub fn begin_import_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        let id = ImportSessionId::new_v7();
        let transaction = self.connection.transaction()?;
        let source_id = upsert_library_source_in_transaction(&transaction, root, now_ms)?;
        transaction.execute(
            "INSERT INTO import_sessions(
                 id, source_id, root_platform, root_native_path, root_display_path,
                 state, started_at_ms, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7)",
            params![
                id.as_bytes().as_slice(),
                source_id.as_bytes().as_slice(),
                root.platform.as_str(),
                root.native_path.as_slice(),
                root.display_path,
                ImportSessionState::Running.as_str(),
                now_ms
            ],
        )?;
        transaction.commit()?;
        Ok(id)
    }

    /// Returns one import session by identifier.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the session query fails or persisted enum
    /// values are invalid.
    pub fn import_session(
        &self,
        id: ImportSessionId,
    ) -> Result<Option<ImportSession>, CatalogError> {
        self.connection
            .query_row(
                "SELECT id, source_id, root_platform, root_native_path, root_display_path,
                        state, started_at_ms, updated_at_ms, finished_at_ms, last_error
                 FROM import_sessions WHERE id = ?1",
                [id.as_bytes().as_slice()],
                read_session,
            )
            .optional()
            .map_err(Into::into)
    }

    /// Lists sessions that were interrupted or failed and may be resumed.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if sessions cannot be queried or decoded.
    pub fn unfinished_import_sessions(&self) -> Result<Vec<ImportSession>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, source_id, root_platform, root_native_path, root_display_path,
                    state, started_at_ms, updated_at_ms, finished_at_ms, last_error
             FROM import_sessions
             WHERE state IN ('running', 'failed')
             ORDER BY updated_at_ms DESC",
        )?;
        let sessions = statement
            .query_map([], read_session)?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(sessions)
    }

    /// Reopens an interrupted or failed session for another idempotent scan.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the session is absent, terminal, or cannot
    /// be updated.
    pub fn resume_import_session(
        &mut self,
        id: ImportSessionId,
        now_ms: i64,
    ) -> Result<ImportSession, CatalogError> {
        let session = self
            .import_session(id)?
            .ok_or(CatalogError::ImportSessionNotFound(id))?;
        if matches!(
            session.state,
            ImportSessionState::Completed | ImportSessionState::Cancelled
        ) {
            return Err(CatalogError::InvalidImportSessionState {
                id,
                state: session.state.as_str(),
            });
        }
        self.connection.execute(
            "UPDATE import_sessions
             SET state = 'running', updated_at_ms = ?2, finished_at_ms = NULL, last_error = NULL
             WHERE id = ?1",
            params![id.as_bytes().as_slice(), now_ms],
        )?;
        self.import_session(id)?
            .ok_or(CatalogError::ImportSessionNotFound(id))
    }

    /// Records a supported file before its asset registration transaction.
    ///
    /// Existing terminal entry results are preserved until the scanner records
    /// the new idempotent registration result.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the journal entry cannot be upserted.
    pub fn record_import_discovered(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<(), CatalogError> {
        let byte_len = i64::try_from(request.byte_len).map_err(|error| {
            CatalogError::Sqlite(rusqlite::Error::ToSqlConversionFailure(Box::new(error)))
        })?;
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "INSERT INTO import_entries(
                 session_id, native_path, display_path, kind, byte_len,
                 modified_at_ms, state, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'discovered', ?7)
             ON CONFLICT(session_id, native_path) DO UPDATE SET
                 display_path = excluded.display_path,
                 kind = excluded.kind,
                 byte_len = excluded.byte_len,
                 modified_at_ms = excluded.modified_at_ms,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                session_id.as_bytes().as_slice(),
                request.location.native_path.as_slice(),
                request.location.display_path,
                request.kind.as_str(),
                byte_len,
                request.modified_at_ms,
                request.now_ms
            ],
        )?;
        touch_session(&transaction, session_id, request.now_ms)?;
        transaction.commit()?;
        Ok(())
    }

    /// Registers an asset and attaches its result to the journal atomically.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the entry is absent or cannot be updated.
    pub fn register_import_asset(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        let transaction = self.connection.transaction()?;
        let result = register_asset_in_transaction(&transaction, request)?;
        let source_id: Option<LibrarySourceId> = transaction
            .query_row(
                "SELECT source_id FROM import_sessions WHERE id = ?1",
                [session_id.as_bytes().as_slice()],
                |row| optional_id(row, 0),
            )
            .optional()?
            .flatten();
        if let Some(source_id) = source_id {
            attach_location_to_library_source(
                &transaction,
                result.location_id,
                source_id,
                request.now_ms,
            )?;
        }
        let updated = transaction.execute(
            "UPDATE import_entries
             SET state = ?3, photo_id = ?4, representation_id = ?5,
                 location_id = ?6, error = NULL, updated_at_ms = ?7
             WHERE session_id = ?1 AND native_path = ?2",
            params![
                session_id.as_bytes().as_slice(),
                request.location.native_path.as_slice(),
                registration_state(result.status),
                result.photo_id.as_bytes().as_slice(),
                result.representation_id.as_bytes().as_slice(),
                result.location_id.as_bytes().as_slice(),
                request.now_ms
            ],
        )?;
        if updated != 1 {
            return Err(CatalogError::ImportEntryNotFound {
                session_id,
                display_path: request.location.display_path.clone(),
            });
        }
        touch_session(&transaction, session_id, request.now_ms)?;
        transaction.commit()?;
        Ok(result)
    }

    /// Adds a filesystem or metadata issue without aborting the whole session.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the issue cannot be recorded.
    pub fn record_import_issue(
        &mut self,
        session_id: ImportSessionId,
        location: &AssetLocation,
        message: &str,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "INSERT INTO import_issues(
                 session_id, native_path, display_path, message, created_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5)",
            params![
                session_id.as_bytes().as_slice(),
                location.native_path.as_slice(),
                location.display_path,
                message,
                now_ms
            ],
        )?;
        touch_session(&transaction, session_id, now_ms)?;
        transaction.commit()?;
        Ok(())
    }

    /// Moves a running session to a terminal state.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if `state` is not terminal, the session does not
    /// exist, or the update fails.
    pub fn finish_import_session(
        &mut self,
        id: ImportSessionId,
        state: ImportSessionState,
        last_error: Option<&str>,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        if !state.is_terminal() {
            return Err(CatalogError::InvalidImportSessionState {
                id,
                state: state.as_str(),
            });
        }
        let updated = self.connection.execute(
            "UPDATE import_sessions
             SET state = ?2, updated_at_ms = ?3, finished_at_ms = ?3, last_error = ?4
             WHERE id = ?1 AND state = 'running'",
            params![id.as_bytes().as_slice(), state.as_str(), now_ms, last_error],
        )?;
        if updated != 1 {
            let session = self
                .import_session(id)?
                .ok_or(CatalogError::ImportSessionNotFound(id))?;
            return Err(CatalogError::InvalidImportSessionState {
                id,
                state: session.state.as_str(),
            });
        }
        Ok(())
    }

    /// Returns a session together with durable entry and issue counts.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the session is absent or summary queries
    /// fail.
    pub fn import_session_summary(
        &self,
        id: ImportSessionId,
    ) -> Result<ImportSessionSummary, CatalogError> {
        let session = self
            .import_session(id)?
            .ok_or(CatalogError::ImportSessionNotFound(id))?;
        let counts: (i64, i64, i64, i64, i64) = self.connection.query_row(
            "SELECT
                 COUNT(*),
                 COALESCE(SUM(state = 'inserted'), 0),
                 COALESCE(SUM(state = 'unchanged'), 0),
                 COALESCE(SUM(state = 'needs_revalidation'), 0),
                 COALESCE(SUM(state = 'failed'), 0)
             FROM import_entries WHERE session_id = ?1",
            [id.as_bytes().as_slice()],
            |row| {
                Ok((
                    row.get(0)?,
                    row.get(1)?,
                    row.get(2)?,
                    row.get(3)?,
                    row.get(4)?,
                ))
            },
        )?;
        let issues: i64 = self.connection.query_row(
            "SELECT COUNT(*) FROM import_issues WHERE session_id = ?1",
            [id.as_bytes().as_slice()],
            |row| row.get(0),
        )?;

        Ok(ImportSessionSummary {
            session,
            discovered: non_negative_count(counts.0)?,
            inserted: non_negative_count(counts.1)?,
            unchanged: non_negative_count(counts.2)?,
            needs_revalidation: non_negative_count(counts.3)?,
            failed_entries: non_negative_count(counts.4)?,
            issues: non_negative_count(issues)?,
        })
    }
}

fn registration_state(status: RegistrationStatus) -> &'static str {
    match status {
        RegistrationStatus::Inserted => "inserted",
        RegistrationStatus::Unchanged => "unchanged",
        RegistrationStatus::NeedsRevalidation => "needs_revalidation",
    }
}

fn touch_session(
    transaction: &rusqlite::Transaction<'_>,
    id: ImportSessionId,
    now_ms: i64,
) -> Result<(), CatalogError> {
    let updated = transaction.execute(
        "UPDATE import_sessions SET updated_at_ms = ?2 WHERE id = ?1 AND state = 'running'",
        params![id.as_bytes().as_slice(), now_ms],
    )?;
    if updated != 1 {
        return Err(CatalogError::ImportSessionNotFound(id));
    }
    Ok(())
}

fn read_session(row: &rusqlite::Row<'_>) -> rusqlite::Result<ImportSession> {
    let platform_text: String = row.get(2)?;
    let state_text: String = row.get(5)?;
    Ok(ImportSession {
        id: read_id(row, 0)?,
        source_id: optional_id(row, 1)?,
        root: AssetLocation::new(
            parse_platform(&platform_text, 2)?,
            row.get(3)?,
            row.get::<_, String>(4)?,
        ),
        state: parse_state(&state_text, 5)?,
        started_at_ms: row.get(6)?,
        updated_at_ms: row.get(7)?,
        finished_at_ms: row.get(8)?,
        last_error: row.get(9)?,
    })
}

fn optional_id<I: EntityId>(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<Option<I>> {
    let bytes: Option<Vec<u8>> = row.get(index)?;
    bytes
        .map(|bytes| {
            let uuid = Uuid::from_slice(&bytes).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
            })?;
            Ok(I::from_uuid(uuid))
        })
        .transpose()
}

fn parse_platform(value: &str, index: usize) -> rusqlite::Result<Platform> {
    match value {
        "macos" => Ok(Platform::MacOs),
        "windows" => Ok(Platform::Windows),
        "other_unix" => Ok(Platform::OtherUnix),
        _ => Err(invalid_text(index, format!("invalid platform: {value}"))),
    }
}

fn parse_state(value: &str, index: usize) -> rusqlite::Result<ImportSessionState> {
    match value {
        "running" => Ok(ImportSessionState::Running),
        "completed" => Ok(ImportSessionState::Completed),
        "failed" => Ok(ImportSessionState::Failed),
        "cancelled" => Ok(ImportSessionState::Cancelled),
        _ => Err(invalid_text(
            index,
            format!("invalid import session state: {value}"),
        )),
    }
}

fn invalid_text(index: usize, message: String) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(
        index,
        Type::Text,
        Box::new(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            message,
        )),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use shadow_domain::{Platform, RepresentationKind};

    fn root() -> AssetLocation {
        AssetLocation::new(Platform::MacOs, b"/photos".to_vec(), "/photos")
    }

    fn request() -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/one.nef".to_vec(),
                "/photos/one.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        }
    }

    #[test]
    fn journal_tracks_registration_and_completion() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let session_id = catalog
            .begin_import_session(&root(), 10)
            .expect("begin session");
        let source_id = catalog
            .import_session(session_id)
            .expect("read session")
            .expect("session exists")
            .source_id
            .expect("v11 source is attached to the session");
        let request = request();
        catalog
            .record_import_discovered(session_id, &request)
            .expect("record discovered");
        catalog
            .register_import_asset(session_id, &request)
            .expect("register journaled asset");
        catalog
            .finish_import_session(session_id, ImportSessionState::Completed, None, 20)
            .expect("finish session");

        let summary = catalog
            .import_session_summary(session_id)
            .expect("session summary");
        assert_eq!(summary.session.state, ImportSessionState::Completed);
        assert_eq!(summary.session.source_id, Some(source_id));
        assert_eq!(summary.discovered, 1);
        assert_eq!(summary.inserted, 1);
        let sources = catalog.library_sources().expect("list scan sources");
        assert_eq!(sources.len(), 1);
        assert_eq!(sources[0].id, source_id);
        assert_eq!(sources[0].root, root());
        assert!(
            catalog
                .unfinished_import_sessions()
                .expect("unfinished sessions")
                .is_empty()
        );
    }

    #[test]
    fn running_session_survives_catalog_reopen() {
        let database_path = std::env::temp_dir().join(format!(
            "shadow-import-journal-{}.sqlite",
            ImportSessionId::new_v7()
        ));
        let session_id;
        {
            let mut catalog = Catalog::open(&database_path).expect("open catalog");
            session_id = catalog
                .begin_import_session(&root(), 10)
                .expect("begin session");
            catalog
                .record_import_discovered(session_id, &request())
                .expect("record discovered");
        }
        {
            let catalog = Catalog::open(&database_path).expect("reopen catalog");
            let sessions = catalog
                .unfinished_import_sessions()
                .expect("unfinished sessions");
            assert_eq!(sessions.len(), 1);
            assert_eq!(sessions[0].id, session_id);
        }

        std::fs::remove_file(&database_path).expect("remove test catalog");
        let wal_path = database_path.with_extension("sqlite-wal");
        let shm_path = database_path.with_extension("sqlite-shm");
        if wal_path.exists() {
            std::fs::remove_file(wal_path).expect("remove test wal");
        }
        if shm_path.exists() {
            std::fs::remove_file(shm_path).expect("remove test shm");
        }
    }

    #[test]
    fn completed_session_cannot_be_rewritten() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let session_id = catalog
            .begin_import_session(&root(), 10)
            .expect("begin session");
        catalog
            .finish_import_session(session_id, ImportSessionState::Completed, None, 20)
            .expect("complete session");

        let error = catalog
            .finish_import_session(session_id, ImportSessionState::Failed, Some("late"), 30)
            .expect_err("terminal session must be immutable");
        assert!(matches!(
            error,
            CatalogError::InvalidImportSessionState { .. }
        ));
        assert_eq!(
            catalog
                .import_session(session_id)
                .expect("load session")
                .expect("session exists")
                .state,
            ImportSessionState::Completed
        );
    }
}

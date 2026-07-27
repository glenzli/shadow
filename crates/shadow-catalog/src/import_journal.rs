use rusqlite::{OptionalExtension, params, types::Type};
use shadow_domain::{
    AssetLocation, EntityId, ImportSessionId, LibrarySourceId, Platform, RepresentationId,
};
use uuid::Uuid;

use crate::library::{
    attach_location_to_identity_match, attach_location_to_library_source, find_identity_match,
    record_content_identity_if_current_in_transaction, upsert_library_source_in_transaction,
};
use crate::{
    Catalog, CatalogError, ContentIdentity, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset, RegisteredAsset, RegistrationStatus,
    RepresentationFingerprint,
};
use crate::{
    asset_registration::{find_existing_asset, register_asset_in_transaction},
    row_codec::{non_negative_count, read_id},
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

/// A non-destructive view of what one completed scan observed for one Library
/// source.
///
/// `not_seen_locations` deliberately does **not** mean a location is globally
/// offline. Sources can overlap, a nested folder can be temporarily
/// unreadable, and the same representation may be available elsewhere. The
/// result is therefore suitable for an explicit relink/review workflow, not a
/// background status mutation.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SourceScanReconciliation {
    pub session_id: ImportSessionId,
    pub source_id: LibrarySourceId,
    pub completed_at_ms: i64,
    pub known_locations: u64,
    pub seen_locations: u64,
    pub not_seen_locations: u64,
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

    /// Starts a durable journal for one explicit source reattach without
    /// promoting the candidate's parent folder into a Library discovery
    /// source. The newly verified location remains usable, but the user keeps
    /// control over which directories are scanned by the Library.
    pub fn begin_relocation_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        let id = ImportSessionId::new_v7();
        self.connection.execute(
            "INSERT INTO import_sessions(
                 id, source_id, root_platform, root_native_path, root_display_path,
                 state, started_at_ms, updated_at_ms
             ) VALUES (?1, NULL, ?2, ?3, ?4, ?5, ?6, ?6)",
            params![
                id.as_bytes().as_slice(),
                root.platform.as_str(),
                root.native_path.as_slice(),
                root.display_path,
                ImportSessionState::Running.as_str(),
                now_ms,
            ],
        )?;
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
        finish_import_registration(&transaction, session_id, request, result)?;
        transaction.commit()?;
        Ok(result)
    }

    /// Attaches one newly discovered location to an already verified,
    /// path-independent representation identity and journals that attachment
    /// in the same transaction.
    ///
    /// This is intentionally stricter than ordinary import registration:
    /// callers must first journal discovery, verify an exact identity outside
    /// the writer, and retain the representation id selected by that proof.
    /// A path that already exists, an absent identity, or an identity owned by
    /// another representation are all rejected without mutating the catalog.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the prior discovery is absent, the target
    /// is not new, or the exact identity no longer proves the expected owner.
    pub fn register_import_verified_relocation(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        identity.validate()?;
        let transaction = self.connection.transaction()?;

        if find_existing_asset(&transaction, &request.location)?.is_some() {
            return Err(CatalogError::RelinkTargetLocationAlreadyRegistered {
                display_path: request.location.display_path.clone(),
            });
        }

        let actual = find_identity_match(&transaction, identity)?.ok_or(
            CatalogError::RelinkIdentityNotRecorded {
                expected_representation_id,
            },
        )?;
        if actual.representation_id != expected_representation_id {
            return Err(CatalogError::RelinkIdentityOwnerMismatch {
                expected_representation_id,
                actual_representation_id: actual.representation_id,
            });
        }

        let result = attach_location_to_identity_match(&transaction, request, actual)?;
        // The lookup above proves the digest has not changed; this only refreshes
        // the observation time and binds it to the newly attached physical
        // source. `attach_location_to_identity_match` changed the
        // representation fingerprint, so persisting the old source stamp here
        // would intentionally be rejected as stale.
        let record = RecordRepresentationContentIdentity {
            representation_id: result.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: request.byte_len,
                modified_at_ms: request.modified_at_ms,
            },
            identity: identity.clone(),
            observed_at_ms: request.now_ms,
        };
        if record_content_identity_if_current_in_transaction(&transaction, &record)?
            == RecordRepresentationContentIdentityStatus::StaleSource
        {
            return Err(CatalogError::ContentIdentitySourceChanged {
                representation_id: result.representation_id,
            });
        }
        finish_import_registration(&transaction, session_id, request, result)?;
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
        let transaction = self.connection.transaction()?;
        let source_id: Option<LibrarySourceId> = transaction
            .query_row(
                "SELECT source_id FROM import_sessions WHERE id = ?1",
                [id.as_bytes().as_slice()],
                |row| optional_id(row, 0),
            )
            .optional()?
            .flatten();
        let updated = transaction.execute(
            "UPDATE import_sessions
             SET state = ?2, updated_at_ms = ?3, finished_at_ms = ?3, last_error = ?4
             WHERE id = ?1 AND state = 'running'",
            params![id.as_bytes().as_slice(), state.as_str(), now_ms, last_error],
        )?;
        if updated != 1 {
            drop(transaction);
            let session = self
                .import_session(id)?
                .ok_or(CatalogError::ImportSessionNotFound(id))?;
            return Err(CatalogError::InvalidImportSessionState {
                id,
                state: session.state.as_str(),
            });
        }
        if state == ImportSessionState::Completed
            && let Some(source_id) = source_id
        {
            transaction.execute(
                "UPDATE library_sources
                 SET last_scanned_at_ms = CASE
                     WHEN last_scanned_at_ms IS NULL OR last_scanned_at_ms < ?2 THEN ?2
                     ELSE last_scanned_at_ms
                 END
                 WHERE id = ?1",
                params![source_id.as_bytes().as_slice(), now_ms],
            )?;
        }
        transaction.commit()?;
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

    /// Reports which known locations were observed by one completed source
    /// scan without changing any location's global availability state.
    ///
    /// The join uses the journaled `location_id`, rather than timestamp
    /// comparisons, so two scans that begin within the same clock tick remain
    /// distinct. A legacy session without a durable Library source has no
    /// reconciliation report.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError::InvalidImportSessionState`] unless the session
    /// completed successfully, or a catalog read error when its durable scan
    /// entries cannot be queried.
    pub fn source_scan_reconciliation(
        &self,
        id: ImportSessionId,
    ) -> Result<Option<SourceScanReconciliation>, CatalogError> {
        let session = self
            .import_session(id)?
            .ok_or(CatalogError::ImportSessionNotFound(id))?;
        if session.state != ImportSessionState::Completed {
            return Err(CatalogError::InvalidImportSessionState {
                id,
                state: session.state.as_str(),
            });
        }
        let Some(source_id) = session.source_id else {
            return Ok(None);
        };

        let (known_locations, seen_locations): (i64, i64) = self.connection.query_row(
            "SELECT COUNT(*), COUNT(seen.location_id)
             FROM location_sources source_locations
             LEFT JOIN (
                 SELECT DISTINCT location_id
                 FROM import_entries
                 WHERE session_id = ?1
                   AND location_id IS NOT NULL
                   AND state IN ('inserted', 'unchanged', 'needs_revalidation')
             ) seen ON seen.location_id = source_locations.location_id
             WHERE source_locations.source_id = ?2",
            params![id.as_bytes().as_slice(), source_id.as_bytes().as_slice()],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )?;
        let known_locations = non_negative_count(known_locations)?;
        let seen_locations = non_negative_count(seen_locations)?;

        Ok(Some(SourceScanReconciliation {
            session_id: id,
            source_id,
            completed_at_ms: session.finished_at_ms.unwrap_or(session.updated_at_ms),
            known_locations,
            seen_locations,
            not_seen_locations: known_locations.saturating_sub(seen_locations),
        }))
    }
}

fn registration_state(status: RegistrationStatus) -> &'static str {
    match status {
        RegistrationStatus::Inserted => "inserted",
        RegistrationStatus::Unchanged => "unchanged",
        RegistrationStatus::NeedsRevalidation => "needs_revalidation",
    }
}

/// Finishes any successful registration by binding its physical location to
/// the import source and replacing the one discovered journal entry. Keeping
/// this beside the journal state machine makes ordinary imports and verified
/// relocations share exactly the same durable completion behavior.
fn finish_import_registration(
    transaction: &rusqlite::Transaction<'_>,
    session_id: ImportSessionId,
    request: &RegisterAsset,
    result: RegisteredAsset,
) -> Result<(), CatalogError> {
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
            transaction,
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
    touch_session(transaction, session_id, request.now_ms)
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
        request_at("/photos/one.nef", 42, Some(100), 1_700_000_000_000)
    }

    fn request_at(
        path: &str,
        byte_len: u64,
        modified_at_ms: Option<i64>,
        now_ms: i64,
    ) -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len,
            modified_at_ms,
            now_ms,
        }
    }

    fn relocated_request() -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/consolidated/2026/one-renamed.nef".to_vec(),
                "/consolidated/2026/one-renamed.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(200),
            now_ms: 1_700_000_000_200,
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
        assert_eq!(sources[0].last_scanned_at_ms, Some(20));
        assert!(
            catalog
                .unfinished_import_sessions()
                .expect("unfinished sessions")
                .is_empty()
        );
    }

    #[test]
    fn explicit_relocation_session_keeps_candidate_out_of_library_sources() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original = catalog
            .register_asset(&request())
            .expect("register original source");
        let identity = ContentIdentity::whole_file_blake3([17; 32]);
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: 42,
                    modified_at_ms: Some(100),
                },
                identity: identity.clone(),
                observed_at_ms: 10,
            })
            .expect("record exact identity");

        let moved_request = relocated_request();
        let session_id = catalog
            .begin_relocation_session(&moved_request.location, 20)
            .expect("begin explicit relocation");
        assert_eq!(
            catalog
                .import_session(session_id)
                .expect("read relocation session")
                .expect("relocation session exists")
                .source_id,
            None
        );
        catalog
            .record_import_discovered(session_id, &moved_request)
            .expect("journal moved candidate");
        catalog
            .register_import_verified_relocation(
                session_id,
                &moved_request,
                original.representation_id,
                &identity,
            )
            .expect("attach exact relocation");
        catalog
            .finish_import_session(session_id, ImportSessionState::Completed, None, 30)
            .expect("complete relocation");

        assert!(
            catalog
                .library_sources()
                .expect("list library sources")
                .is_empty()
        );
    }

    #[test]
    fn source_freshness_is_recorded_only_after_completed_scan() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let cancelled = catalog
            .begin_import_session(&root(), 10)
            .expect("begin cancelled scan");
        let source_id = catalog
            .import_session(cancelled)
            .expect("read cancelled session")
            .expect("cancelled session exists")
            .source_id
            .expect("source attached");
        assert_eq!(
            catalog.library_sources().expect("sources")[0].last_scanned_at_ms,
            None
        );

        catalog
            .finish_import_session(cancelled, ImportSessionState::Cancelled, None, 20)
            .expect("cancel scan");
        assert_eq!(
            catalog.library_sources().expect("sources")[0].last_scanned_at_ms,
            None
        );

        let failed = catalog
            .begin_import_session(&root(), 30)
            .expect("begin failed scan");
        catalog
            .finish_import_session(failed, ImportSessionState::Failed, Some("unavailable"), 40)
            .expect("fail scan");
        assert_eq!(
            catalog.library_sources().expect("sources")[0].last_scanned_at_ms,
            None
        );

        let completed = catalog
            .begin_import_session(&root(), 50)
            .expect("begin completed scan");
        let observed = request_at("/photos/one.nef", 42, Some(100), 51);
        catalog
            .record_import_discovered(completed, &observed)
            .expect("record observed file");
        catalog
            .register_import_asset(completed, &observed)
            .expect("register observed file");
        catalog
            .finish_import_session(completed, ImportSessionState::Completed, None, 60)
            .expect("complete scan");

        let source = catalog
            .library_sources()
            .expect("sources")
            .into_iter()
            .find(|source| source.id == source_id)
            .expect("same source");
        assert_eq!(source.last_scanned_at_ms, Some(60));
    }

    #[test]
    fn completed_scan_reports_source_locations_not_seen_without_marking_them_offline() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let first = catalog
            .begin_import_session(&root(), 10)
            .expect("begin initial scan");
        for observed in [
            request_at("/photos/one.nef", 42, Some(100), 11),
            request_at("/photos/two.nef", 84, Some(101), 12),
        ] {
            catalog
                .record_import_discovered(first, &observed)
                .expect("record initial source file");
            catalog
                .register_import_asset(first, &observed)
                .expect("register initial source file");
        }
        catalog
            .finish_import_session(first, ImportSessionState::Completed, None, 20)
            .expect("complete initial scan");
        assert_eq!(
            catalog
                .source_scan_reconciliation(first)
                .expect("reconcile initial scan"),
            Some(SourceScanReconciliation {
                session_id: first,
                source_id: catalog
                    .import_session(first)
                    .expect("read initial session")
                    .expect("initial session exists")
                    .source_id
                    .expect("source attached"),
                completed_at_ms: 20,
                known_locations: 2,
                seen_locations: 2,
                not_seen_locations: 0,
            })
        );

        let second = catalog
            .begin_import_session(&root(), 30)
            .expect("begin follow-up scan");
        let observed = request_at("/photos/one.nef", 42, Some(100), 31);
        catalog
            .record_import_discovered(second, &observed)
            .expect("record remaining source file");
        catalog
            .register_import_asset(second, &observed)
            .expect("register remaining source file");
        catalog
            .finish_import_session(second, ImportSessionState::Completed, None, 40)
            .expect("complete follow-up scan");

        let reconciliation = catalog
            .source_scan_reconciliation(second)
            .expect("reconcile completed scan")
            .expect("source-backed session");
        assert_eq!(reconciliation.known_locations, 2);
        assert_eq!(reconciliation.seen_locations, 1);
        assert_eq!(reconciliation.not_seen_locations, 1);
        assert_eq!(
            catalog
                .library_photo_count(&crate::LibraryPhotoFilter::default())
                .expect("unseen source location remains visible"),
            2
        );
    }

    #[test]
    fn source_reconciliation_requires_a_completed_session() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let running = catalog
            .begin_import_session(&root(), 10)
            .expect("begin scan");
        assert!(matches!(
            catalog.source_scan_reconciliation(running),
            Err(CatalogError::InvalidImportSessionState {
                id,
                state: "running"
            }) if id == running
        ));
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

    #[test]
    fn verified_relocation_attaches_the_existing_representation_and_journals_once() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original = catalog
            .register_asset(&request())
            .expect("register original");
        let identity = ContentIdentity::whole_file_blake3([23; 32]);
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: 42,
                    modified_at_ms: Some(100),
                },
                identity: identity.clone(),
                observed_at_ms: 10,
            })
            .expect("record exact identity");

        let session_id = catalog
            .begin_import_session(&root(), 20)
            .expect("begin relocation session");
        let moved_request = relocated_request();
        catalog
            .record_import_discovered(session_id, &moved_request)
            .expect("journal relocated discovery");

        let moved = catalog
            .register_import_verified_relocation(
                session_id,
                &moved_request,
                original.representation_id,
                &identity,
            )
            .expect("attach verified relocation");

        assert_eq!(moved.photo_id, original.photo_id);
        assert_eq!(moved.representation_id, original.representation_id);
        assert_ne!(moved.location_id, original.location_id);
        assert_eq!(moved.status, RegistrationStatus::NeedsRevalidation);
        assert_eq!(catalog.stats().expect("stats").photos, 1);
        assert_eq!(catalog.stats().expect("stats").representations, 1);
        assert_eq!(catalog.stats().expect("stats").locations, 2);
        assert_eq!(
            catalog
                .relink_match(&identity)
                .expect("identity stays current after move"),
            Some(crate::RelinkMatch {
                photo_id: original.photo_id,
                representation_id: original.representation_id,
            })
        );
        assert_eq!(
            catalog
                .import_session_summary(session_id)
                .expect("journal summary")
                .needs_revalidation,
            1
        );
    }

    #[test]
    fn verified_relocation_rejects_stale_identity_or_registered_target_without_side_effects() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original = catalog
            .register_asset(&request())
            .expect("register original");
        let identity = ContentIdentity::whole_file_blake3([29; 32]);
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: 42,
                    modified_at_ms: Some(100),
                },
                identity: identity.clone(),
                observed_at_ms: 10,
            })
            .expect("record exact identity");

        let session_id = catalog
            .begin_import_session(&root(), 20)
            .expect("begin relocation session");
        let moved_request = relocated_request();
        catalog
            .record_import_discovered(session_id, &moved_request)
            .expect("journal relocated discovery");

        let wrong_representation = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/unrelated.nef".to_vec(),
                    "/photos/unrelated.nef",
                ),
                byte_len: 7,
                modified_at_ms: Some(11),
                now_ms: 21,
            })
            .expect("register unrelated asset");
        let mismatch = catalog
            .register_import_verified_relocation(
                session_id,
                &moved_request,
                wrong_representation.representation_id,
                &identity,
            )
            .expect_err("another representation must not acquire identity owner location");
        assert!(matches!(
            mismatch,
            CatalogError::RelinkIdentityOwnerMismatch {
                expected_representation_id,
                actual_representation_id,
            } if expected_representation_id == wrong_representation.representation_id
                && actual_representation_id == original.representation_id
        ));
        assert_eq!(catalog.stats().expect("stats").locations, 2);

        let existing_target = catalog
            .register_import_asset(session_id, &moved_request)
            .expect("ordinary registration occupies target");
        let conflict = catalog
            .register_import_verified_relocation(
                session_id,
                &moved_request,
                original.representation_id,
                &identity,
            )
            .expect_err("pre-existing target must not be merged by relocation");
        assert!(matches!(
            conflict,
            CatalogError::RelinkTargetLocationAlreadyRegistered { .. }
        ));
        assert_eq!(catalog.stats().expect("stats").locations, 3);
        assert_ne!(
            existing_target.representation_id,
            original.representation_id
        );
    }

    #[test]
    fn verified_relocation_rejects_a_confirmation_after_its_original_source_changes() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original_request = request();
        let original = catalog
            .register_asset(&original_request)
            .expect("register original");
        let identity = ContentIdentity::whole_file_blake3([37; 32]);
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: original_request.byte_len,
                    modified_at_ms: original_request.modified_at_ms,
                },
                identity: identity.clone(),
                observed_at_ms: 10,
            })
            .expect("record original identity");

        let session_id = catalog
            .begin_import_session(&root(), 20)
            .expect("begin relocation session");
        let moved_request = relocated_request();
        catalog
            .record_import_discovered(session_id, &moved_request)
            .expect("journal relocation discovery");

        catalog
            .register_asset(&request_at("/photos/one.nef", 43, Some(101), 21))
            .expect("observe original source replacement");
        let error = catalog
            .register_import_verified_relocation(
                session_id,
                &moved_request,
                original.representation_id,
                &identity,
            )
            .expect_err("invalidated identity cannot attach the confirmed move");
        assert!(matches!(
            error,
            CatalogError::RelinkIdentityNotRecorded {
                expected_representation_id
            } if expected_representation_id == original.representation_id
        ));
        assert_eq!(catalog.stats().expect("stats").locations, 1);
    }
}

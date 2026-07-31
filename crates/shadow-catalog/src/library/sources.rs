//! Library source discovery, exact content identity, and relocation transactions.
//!
//! Paths are observations rather than photo identity. This module owns the lifecycle that attaches
//! changing locations to stable representations and records scan-specific source health.

use rusqlite::{OptionalExtension, Transaction, params, params_from_iter, types::Value};
use shadow_domain::{
    AssetLocation, EntityId, ImportSessionId, LibrarySourceId, LocationId, RepresentationId,
};

use crate::{
    Catalog, CatalogError, RegisterAsset, RegisteredAsset, RepresentationFingerprint,
    asset_registration::{
        find_existing_asset, insert_asset, location_file_name_sort_key,
        register_asset_in_transaction,
    },
    decode_snapshot::representation_fingerprint_in_transaction,
    row_codec::read_id,
};

use super::{
    ContentIdentity, LibrarySourceHealth, LibrarySourceRecord, MAX_LIBRARY_PAGE_SIZE,
    MissingSourceLocationCursor, MissingSourceLocationPage, MissingSourceRelinkTarget,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RelinkMatch,
    rows::{read_library_source, read_library_source_health, read_missing_source_location},
};

impl Catalog {
    /// Records a versioned content identity for an existing representation.
    ///
    /// A single exact identity is globally owned by one representation. This
    /// deliberately prevents a weak fingerprint from silently merging two
    /// photos; callers should only use an identity after its stated domain has
    /// been strongly verified.
    pub fn record_representation_content_identity(
        &mut self,
        request: &RecordRepresentationContentIdentity,
    ) -> Result<RecordRepresentationContentIdentityStatus, CatalogError> {
        let transaction = self.connection.transaction()?;
        let status = record_content_identity_if_current_in_transaction(&transaction, request)?;
        if status == RecordRepresentationContentIdentityStatus::StaleSource {
            return Ok(status);
        }
        transaction.commit()?;
        Ok(status)
    }

    /// Finds the one representation that owns an exact identity.
    pub fn relink_match(
        &self,
        identity: &ContentIdentity,
    ) -> Result<Option<RelinkMatch>, CatalogError> {
        identity.validate()?;
        self.connection
            .query_row(
                "SELECT r.photo_id, r.id
                 FROM representation_content_identities i
                 JOIN representations r ON r.id = i.representation_id
                 WHERE i.scope = ?1 AND i.algorithm = ?2
                   AND i.provider_id = ?3 AND i.provider_version = ?4 AND i.digest = ?5
                   AND i.source_byte_len = r.byte_len
                   AND i.source_modified_at_ms IS r.modified_at_ms",
                params![
                    identity.scope.as_str(),
                    identity.algorithm,
                    identity.provider_id,
                    identity.provider_version,
                    identity.digest.as_slice(),
                ],
                |row| {
                    Ok(RelinkMatch {
                        photo_id: read_id(row, 0)?,
                        representation_id: read_id(row, 1)?,
                    })
                },
            )
            .optional()
            .map_err(Into::into)
    }

    /// Registers a physical asset, reusing an existing logical representation
    /// only for a verified exact content identity.
    ///
    /// This is intentionally separate from normal scanning. Full-file and
    /// decoder mosaic hashing are expensive and should be scheduled only for a
    /// relocation candidate or background idle task, not every initial import.
    pub fn register_asset_with_content_identity(
        &mut self,
        request: &RegisterAsset,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        identity.validate()?;
        let transaction = self.connection.transaction()?;
        let result = if find_existing_asset(&transaction, &request.location)?.is_some() {
            register_asset_in_transaction(&transaction, request)?
        } else if let Some(existing) = find_identity_match(&transaction, identity)? {
            attach_location_to_identity_match(&transaction, request, existing)?
        } else {
            insert_asset(&transaction, request)?
        };
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
        transaction.commit()?;
        Ok(result)
    }

    /// Lists configured scan sources. Source records are durable even if all
    /// their current locations become unavailable.
    pub fn library_sources(&self) -> Result<Vec<LibrarySourceRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, platform, native_path, display_path, enabled, created_at_ms, last_scanned_at_ms
             FROM library_sources
             WHERE enabled = 1
             ORDER BY display_path COLLATE NOCASE, id",
        )?;
        let rows = statement.query_map([], read_library_source)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Disables one configured discovery root without deleting any photo,
    /// representation, location, edit, or file on disk.
    ///
    /// Source-to-location evidence is retained so the Library can hide photos
    /// that are available only through this root and restore them with their
    /// edits if the same root is added again. A running scan must finish or
    /// cancel first.
    pub fn remove_library_source(
        &mut self,
        source_id: LibrarySourceId,
    ) -> Result<bool, CatalogError> {
        self.remove_library_source_with_legacy_roots(source_id, &[], 0)
    }

    /// Disables a discovery root after adopting unowned legacy locations
    /// beneath any equivalent filesystem roots supplied by the caller.
    ///
    /// Older imports predate durable source ownership, and the same folder may
    /// have been observed once through a symlink and once through its resolved
    /// path. Associating only currently unowned locations keeps removal
    /// reversible without stealing a location from another configured source.
    pub fn remove_library_source_with_legacy_roots(
        &mut self,
        source_id: LibrarySourceId,
        legacy_roots: &[AssetLocation],
        observed_at_ms: i64,
    ) -> Result<bool, CatalogError> {
        let transaction = self.connection.transaction()?;
        let enabled = transaction.query_row(
            "SELECT EXISTS(
                 SELECT 1 FROM library_sources WHERE id = ?1 AND enabled = 1
             )",
            [source_id.as_bytes().as_slice()],
            |row| row.get::<_, bool>(0),
        )?;
        if !enabled {
            return Ok(false);
        }
        let has_running_scan = transaction.query_row(
            "SELECT EXISTS(
                 SELECT 1 FROM import_sessions
                 WHERE source_id = ?1 AND state = 'running'
             )",
            [source_id.as_bytes().as_slice()],
            |row| row.get::<_, bool>(0),
        )?;
        if has_running_scan {
            return Err(CatalogError::InvalidLibraryQuery(
                "a Library folder cannot be removed while it is being scanned".into(),
            ));
        }
        for root in legacy_roots {
            attach_unowned_locations_beneath_root(&transaction, source_id, root, observed_at_ms)?;
        }
        transaction.execute(
            "UPDATE library_sources SET enabled = 0 WHERE id = ?1",
            [source_id.as_bytes().as_slice()],
        )?;
        transaction.commit()?;
        Ok(true)
    }

    /// Lists configured sources together with their latest completed-scan
    /// evidence. Source health is intentionally observational: it never flips
    /// a location's global availability state.
    pub fn library_source_health(&self) -> Result<Vec<LibrarySourceHealth>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT s.id, s.platform, s.native_path, s.display_path,
                    s.enabled, s.created_at_ms, s.last_scanned_at_ms,
                    latest.id, latest.finished_at_ms,
                    (
                        SELECT COUNT(*) FROM location_sources source_locations
                        WHERE source_locations.source_id = s.id
                    ),
                    (
                        SELECT COUNT(*)
                        FROM location_sources source_locations
                        JOIN import_entries seen
                          ON seen.location_id = source_locations.location_id
                         AND seen.session_id = latest.id
                         AND seen.state IN ('inserted', 'unchanged', 'needs_revalidation')
                        WHERE source_locations.source_id = s.id
                    )
             FROM library_sources s
             LEFT JOIN import_sessions latest ON latest.id = (
                 SELECT candidate.id
                 FROM import_sessions candidate
                 WHERE candidate.source_id = s.id
                   AND candidate.state = 'completed'
                 ORDER BY candidate.finished_at_ms DESC, candidate.id DESC
                 LIMIT 1
             )
             WHERE s.enabled = 1
             ORDER BY s.display_path COLLATE NOCASE, s.id",
        )?;
        let rows = statement.query_map([], read_library_source_health)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Pages the locations that a specific completed scan did not observe.
    ///
    /// The completed `scan_session_id` is an explicit consistency boundary.
    /// Callers should obtain it from [`LibrarySourceHealth`], retain it while a
    /// review is open, and start a new review after choosing a later scan.
    /// A legacy import session without a Library source returns `None`.
    pub fn missing_source_location_page(
        &self,
        scan_session_id: ImportSessionId,
        after: Option<&MissingSourceLocationCursor>,
        requested_limit: usize,
    ) -> Result<Option<MissingSourceLocationPage>, CatalogError> {
        let Some(reconciliation) = self.source_scan_reconciliation(scan_session_id)? else {
            return Ok(None);
        };
        let page_size = requested_limit.clamp(1, MAX_LIBRARY_PAGE_SIZE);
        let mut sql = String::from(
            "SELECT p.id, r.id, l.id, l.platform, l.native_path, l.display_path,
                    r.kind, r.byte_len, r.modified_at_ms,
                    f.captured_at_unix_seconds, f.camera_key,
                    source_locations.last_seen_at_ms
             FROM location_sources source_locations
             JOIN locations l ON l.id = source_locations.location_id
             JOIN representations r ON r.id = l.representation_id
             JOIN photos p ON p.id = r.photo_id
             LEFT JOIN photo_library_facts f ON f.photo_id = p.id
             WHERE source_locations.source_id = ?1
               AND NOT EXISTS (
                   SELECT 1 FROM import_entries seen
                   WHERE seen.session_id = ?2
                     AND seen.location_id = source_locations.location_id
                     AND seen.state IN ('inserted', 'unchanged', 'needs_revalidation')
               )",
        );
        let mut values = vec![
            Value::Blob(reconciliation.source_id.as_bytes().to_vec()),
            Value::Blob(scan_session_id.as_bytes().to_vec()),
        ];
        if let Some(cursor) = after {
            sql.push_str(" AND source_locations.location_id < ?");
            values.push(Value::Blob(cursor.location_id.as_bytes().to_vec()));
        }
        sql.push_str(
            " ORDER BY source_locations.location_id DESC
              LIMIT ?",
        );
        values.push(Value::Integer(
            i64::try_from(page_size + 1).unwrap_or(i64::MAX),
        ));

        let mut statement = self.connection.prepare(&sql)?;
        let rows = statement.query_map(
            params_from_iter(values.iter()),
            read_missing_source_location,
        )?;
        let mut items = rows.collect::<rusqlite::Result<Vec<_>>>()?;
        let has_more = items.len() > page_size;
        items.truncate(page_size);
        let next_cursor = if has_more {
            items.last().map(|last| MissingSourceLocationCursor {
                location_id: last.location_id,
            })
        } else {
            None
        };
        Ok(Some(MissingSourceLocationPage {
            reconciliation,
            items,
            next_cursor,
        }))
    }

    /// Returns one original location that was absent from a particular
    /// completed source scan. This is the read boundary for an explicit
    /// user-confirmed reattach; it never guesses from a path or metadata.
    pub fn missing_source_relink_target(
        &self,
        scan_session_id: ImportSessionId,
        location_id: LocationId,
    ) -> Result<Option<MissingSourceRelinkTarget>, CatalogError> {
        let Some(reconciliation) = self.source_scan_reconciliation(scan_session_id)? else {
            return Ok(None);
        };
        self.connection
            .query_row(
                "SELECT p.id, r.id, l.id, l.platform, l.native_path, l.display_path,
                        r.kind, r.byte_len, r.modified_at_ms,
                        f.captured_at_unix_seconds, f.camera_key,
                        source_locations.last_seen_at_ms
                 FROM location_sources source_locations
                 JOIN locations l ON l.id = source_locations.location_id
                 JOIN representations r ON r.id = l.representation_id
                 JOIN photos p ON p.id = r.photo_id
                 LEFT JOIN photo_library_facts f ON f.photo_id = p.id
                 WHERE source_locations.source_id = ?1
                   AND source_locations.location_id = ?2
                   AND NOT EXISTS (
                       SELECT 1 FROM import_entries seen
                       WHERE seen.session_id = ?3
                         AND seen.location_id = source_locations.location_id
                         AND seen.state IN ('inserted', 'unchanged', 'needs_revalidation')
                   )",
                params![
                    reconciliation.source_id.as_bytes().as_slice(),
                    location_id.as_bytes().as_slice(),
                    scan_session_id.as_bytes().as_slice(),
                ],
                |row| {
                    Ok(MissingSourceRelinkTarget {
                        location: read_missing_source_location(row)?,
                    })
                },
            )
            .optional()
            .map_err(Into::into)
    }

    /// Returns one active original location selected directly from the
    /// photo-first Library for an explicit user-confirmed reattach.
    ///
    /// Unlike [`Self::missing_source_relink_target`], this boundary does not
    /// require a completed scan: a disconnected drive or externally moved
    /// folder can make the current grid location unreachable before another
    /// scan has produced absence evidence. Exact content verification still
    /// decides whether the candidate may be attached.
    pub fn library_source_relink_target(
        &self,
        location_id: LocationId,
    ) -> Result<Option<MissingSourceRelinkTarget>, CatalogError> {
        self.connection
            .query_row(
                "SELECT p.id, r.id, l.id, l.platform, l.native_path, l.display_path,
                        r.kind, r.byte_len, r.modified_at_ms,
                        f.captured_at_unix_seconds, f.camera_key,
                        COALESCE((
                            SELECT MAX(source_locations.last_seen_at_ms)
                            FROM location_sources source_locations
                            WHERE source_locations.location_id = l.id
                        ), l.created_at_ms)
                 FROM locations l
                 JOIN representations r ON r.id = l.representation_id
                 JOIN photos p ON p.id = r.photo_id
                 LEFT JOIN photo_library_facts f ON f.photo_id = p.id
                 WHERE l.id = ?1
                   AND p.lifecycle_state = 'active'
                   AND r.kind IN ('original_raw', 'original_raster')
                   AND l.status = 'online'
                   AND (
                       NOT EXISTS (
                           SELECT 1 FROM location_sources ownership
                           WHERE ownership.location_id = l.id
                       )
                       OR EXISTS (
                           SELECT 1
                           FROM location_sources ownership
                           JOIN library_sources source
                             ON source.id = ownership.source_id
                            AND source.enabled = 1
                           WHERE ownership.location_id = l.id
                       )
                   )",
                params![location_id.as_bytes().as_slice()],
                |row| {
                    Ok(MissingSourceRelinkTarget {
                        location: read_missing_source_location(row)?,
                    })
                },
            )
            .optional()
            .map_err(Into::into)
    }
}

fn attach_unowned_locations_beneath_root(
    transaction: &Transaction<'_>,
    source_id: LibrarySourceId,
    root: &AssetLocation,
    observed_at_ms: i64,
) -> rusqlite::Result<()> {
    if root.native_path.is_empty() {
        return Ok(());
    }
    let descendant_prefix = descendant_prefix(root);
    transaction.execute(
        "INSERT INTO location_sources(location_id, source_id, first_seen_at_ms, last_seen_at_ms)
         SELECT location.id, ?1, ?5, ?5
         FROM locations location
         WHERE location.platform = ?2
           AND (
               location.native_path = ?3
               OR substr(location.native_path, 1, length(?4)) = ?4
           )
           AND NOT EXISTS (
               SELECT 1 FROM location_sources ownership
               WHERE ownership.location_id = location.id
           )",
        params![
            source_id.as_bytes().as_slice(),
            root.platform.as_str(),
            root.native_path.as_slice(),
            descendant_prefix,
            observed_at_ms,
        ],
    )?;
    Ok(())
}

fn descendant_prefix(root: &AssetLocation) -> Vec<u8> {
    let mut prefix = root.native_path.clone();
    match root.platform {
        shadow_domain::Platform::Windows => {
            let ends_with_separator = prefix.ends_with(&[b'\\', 0]) || prefix.ends_with(&[b'/', 0]);
            if !ends_with_separator {
                prefix.extend_from_slice(&[b'\\', 0]);
            }
        }
        shadow_domain::Platform::MacOs | shadow_domain::Platform::OtherUnix => {
            if !prefix.ends_with(b"/") {
                prefix.push(b'/');
            }
        }
    }
    prefix
}

pub(crate) fn upsert_library_source_in_transaction(
    transaction: &Transaction<'_>,
    root: &AssetLocation,
    now_ms: i64,
) -> rusqlite::Result<LibrarySourceId> {
    let existing = transaction
        .query_row(
            "SELECT id FROM library_sources WHERE platform = ?1 AND native_path = ?2",
            params![root.platform.as_str(), root.native_path.as_slice()],
            |row| read_id::<LibrarySourceId>(row, 0),
        )
        .optional()?;
    if let Some(id) = existing {
        transaction.execute(
            "UPDATE library_sources SET display_path = ?2, enabled = 1
             WHERE id = ?1",
            params![id.as_bytes().as_slice(), root.display_path],
        )?;
        return Ok(id);
    }

    let id = LibrarySourceId::new_v7();
    transaction.execute(
        "INSERT INTO library_sources(
             id, platform, native_path, display_path, enabled, created_at_ms, last_scanned_at_ms
         ) VALUES (?1, ?2, ?3, ?4, 1, ?5, NULL)",
        params![
            id.as_bytes().as_slice(),
            root.platform.as_str(),
            root.native_path.as_slice(),
            root.display_path,
            now_ms,
        ],
    )?;
    Ok(id)
}

pub(crate) fn attach_location_to_library_source(
    transaction: &Transaction<'_>,
    location_id: LocationId,
    source_id: LibrarySourceId,
    now_ms: i64,
) -> rusqlite::Result<()> {
    transaction.execute(
        "INSERT INTO location_sources(location_id, source_id, first_seen_at_ms, last_seen_at_ms)
         VALUES (?1, ?2, ?3, ?3)
         ON CONFLICT(location_id, source_id) DO UPDATE SET last_seen_at_ms = excluded.last_seen_at_ms",
        params![
            location_id.as_bytes().as_slice(),
            source_id.as_bytes().as_slice(),
            now_ms,
        ],
    )?;
    Ok(())
}

pub(crate) fn record_content_identity_if_current_in_transaction(
    transaction: &Transaction<'_>,
    request: &RecordRepresentationContentIdentity,
) -> Result<RecordRepresentationContentIdentityStatus, CatalogError> {
    request.identity.validate()?;
    let current =
        representation_fingerprint_in_transaction(transaction, request.representation_id)?;
    if current != request.expected_source {
        return Ok(RecordRepresentationContentIdentityStatus::StaleSource);
    }

    upsert_content_identity(
        transaction,
        request.representation_id,
        current,
        &request.identity,
        request.observed_at_ms,
    )?;
    Ok(RecordRepresentationContentIdentityStatus::Recorded)
}

pub(crate) fn upsert_content_identity(
    transaction: &Transaction<'_>,
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    identity: &ContentIdentity,
    observed_at_ms: i64,
) -> rusqlite::Result<()> {
    let source_byte_len = i64::try_from(source.byte_len)
        .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;
    transaction.execute(
        "INSERT INTO representation_content_identities(
             representation_id, scope, algorithm, provider_id, provider_version, digest,
             source_byte_len, source_modified_at_ms, observed_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
         ON CONFLICT(representation_id, scope, algorithm, provider_id, provider_version)
         DO UPDATE SET digest = excluded.digest,
                       source_byte_len = excluded.source_byte_len,
                       source_modified_at_ms = excluded.source_modified_at_ms,
                       observed_at_ms = excluded.observed_at_ms",
        params![
            representation_id.as_bytes().as_slice(),
            identity.scope.as_str(),
            identity.algorithm,
            identity.provider_id,
            identity.provider_version,
            identity.digest.as_slice(),
            source_byte_len,
            source.modified_at_ms,
            observed_at_ms,
        ],
    )?;
    Ok(())
}

pub(crate) fn find_identity_match(
    transaction: &Transaction<'_>,
    identity: &ContentIdentity,
) -> rusqlite::Result<Option<RelinkMatch>> {
    transaction
        .query_row(
            "SELECT r.photo_id, r.id
             FROM representation_content_identities i
             JOIN representations r ON r.id = i.representation_id
             WHERE i.scope = ?1 AND i.algorithm = ?2
               AND i.provider_id = ?3 AND i.provider_version = ?4 AND i.digest = ?5
               AND i.source_byte_len = r.byte_len
               AND i.source_modified_at_ms IS r.modified_at_ms",
            params![
                identity.scope.as_str(),
                identity.algorithm,
                identity.provider_id,
                identity.provider_version,
                identity.digest.as_slice(),
            ],
            |row| {
                Ok(RelinkMatch {
                    photo_id: read_id(row, 0)?,
                    representation_id: read_id(row, 1)?,
                })
            },
        )
        .optional()
}

pub(crate) fn attach_location_to_identity_match(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
    existing: RelinkMatch,
) -> rusqlite::Result<RegisteredAsset> {
    let location_id = LocationId::new_v7();
    transaction.execute(
        "INSERT INTO locations(
             id, representation_id, platform, native_path, display_path, sort_name_key,
             status, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'online', ?7)",
        params![
            location_id.as_bytes().as_slice(),
            existing.representation_id.as_bytes().as_slice(),
            request.location.platform.as_str(),
            request.location.native_path.as_slice(),
            request.location.display_path,
            location_file_name_sort_key(&request.location.display_path),
            request.now_ms,
        ],
    )?;
    // The new location is an exact match for the declared identity but might
    // carry a different embedded preview or EXIF. Update the physical source
    // stamp so stale decoder/cache products are naturally revalidated; the
    // photo id, edits, ratings, likes, and album memberships remain intact.
    transaction.execute(
        "UPDATE representations SET byte_len = ?2, modified_at_ms = ?3
         WHERE id = ?1",
        params![
            existing.representation_id.as_bytes().as_slice(),
            i64::try_from(request.byte_len)
                .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?,
            request.modified_at_ms,
        ],
    )?;
    Ok(RegisteredAsset {
        photo_id: existing.photo_id,
        representation_id: existing.representation_id,
        location_id,
        // Existing scan/report plumbing already understands this status as a
        // refresh request. It is deliberately not reported as a fresh import.
        status: crate::RegistrationStatus::NeedsRevalidation,
    })
}

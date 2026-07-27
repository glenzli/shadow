//! Photo-first Library projections, collections, and path-independent source identity.
//!
//! The catalog's foundational `photos -> representations -> locations` graph remains the
//! authoritative model. This module adds the durable projections needed to browse that graph at
//! scale without making an import directory the owner of a photo. All cache payloads remain in
//! `shadow-cache`; SQLite stores only small, indexed facts and references.

use rusqlite::{OptionalExtension, Transaction, params, params_from_iter, types::Value};
use shadow_domain::{
    AssetLocation, CollectionId, EntityId, ImportSessionId, LibrarySourceId, LocationId, PhotoId,
    RepresentationId,
};

use crate::{
    Catalog, CatalogError, RegisterAsset, RegisteredAsset, RepresentationFingerprint,
    decode_snapshot::representation_fingerprint_in_transaction, find_existing_asset, insert_asset,
    read_id,
};

mod browse;
mod model;
mod rows;

pub use model::{
    AlbumKind, AlbumRecord, ContentIdentity, ContentIdentityScope, LibraryApertureRange,
    LibraryDateRange, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryFacetValue,
    LibraryPhotoCursor, LibraryPhotoFacts, LibraryPhotoFilter, LibraryPhotoPage,
    LibraryPhotoRecord, LibrarySourceHealth, LibrarySourceRecord, MAX_LIBRARY_FACET_PAGE_SIZE,
    MAX_LIBRARY_PAGE_SIZE, MissingSourceLocationCursor, MissingSourceLocationPage,
    MissingSourceLocationRecord, MissingSourceRelinkTarget, PhotoLibraryState,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RelinkMatch,
    SetPhotoLibraryState, SmartAlbumQueryV1, library_equipment_key,
};
use model::{normalized_equipment_key, validate_album, validate_facts, validate_library_state};
use rows::{
    read_album, read_library_facts, read_library_source, read_library_source_health,
    read_missing_source_location,
};

#[cfg(test)]
mod tests;

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
            super::register_asset_in_transaction(&transaction, request)?
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

    /// Persists filterable metadata as a compact, indexed read model.
    pub fn upsert_photo_library_facts(
        &mut self,
        facts: &LibraryPhotoFacts,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        upsert_photo_library_facts_in_transaction(&transaction, facts)?;
        transaction.commit()?;
        Ok(())
    }

    /// Returns a photo's indexed Library facts when metadata has been decoded.
    pub fn photo_library_facts(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<LibraryPhotoFacts>, CatalogError> {
        self.connection
            .query_row(
                "SELECT photo_id, captured_at_unix_seconds, capture_day,
                        camera_make, camera_model, lens_make, lens_model,
                        aperture_milli, focal_length_tenth_mm, iso_speed,
                        latitude_e7, longitude_e7, place_name,
                        indexed_representation_id, indexed_source_byte_len,
                        indexed_source_modified_at_ms, indexed_at_ms
                 FROM photo_library_facts WHERE photo_id = ?1",
                [photo_id.as_bytes().as_slice()],
                read_library_facts,
            )
            .optional()
            .map_err(Into::into)
    }

    /// Replaces the independent like/color state for one photo.
    pub fn set_photo_library_state(
        &mut self,
        state: &SetPhotoLibraryState,
    ) -> Result<(), CatalogError> {
        validate_library_state(state)?;
        let transaction = self.connection.transaction()?;
        ensure_photo_exists(&transaction, state.photo_id)?;
        transaction.execute(
            "INSERT INTO photo_library_state(photo_id, liked, color_label, updated_at_ms)
             VALUES (?1, ?2, ?3, ?4)
             ON CONFLICT(photo_id) DO UPDATE SET
                 liked = excluded.liked,
                 color_label = excluded.color_label,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                state.photo_id.as_bytes().as_slice(),
                i64::from(state.liked),
                state.color_label.trim().to_ascii_lowercase(),
                state.updated_at_ms,
            ],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Reads current like/color state; absent rows are the neutral default.
    pub fn photo_library_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryState, CatalogError> {
        ensure_photo_exists_connection(&self.connection, photo_id)?;
        self.connection
            .query_row(
                "SELECT liked, color_label, updated_at_ms
                 FROM photo_library_state WHERE photo_id = ?1",
                [photo_id.as_bytes().as_slice()],
                |row| {
                    Ok(PhotoLibraryState {
                        photo_id,
                        liked: row.get::<_, i64>(0)? != 0,
                        color_label: row.get(1)?,
                        updated_at_ms: row.get(2)?,
                    })
                },
            )
            .optional()?
            .map_or_else(
                || {
                    Ok(PhotoLibraryState {
                        photo_id,
                        liked: false,
                        color_label: "none".to_owned(),
                        updated_at_ms: 0,
                    })
                },
                Ok,
            )
    }

    /// Creates one user-visible album. A photo may belong to any number of
    /// manual albums through [`Self::add_photo_to_album`].
    pub fn create_library_album(
        &mut self,
        kind: AlbumKind,
        name: &str,
        query_json: Option<&str>,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let (name, query_json) = validate_album(kind, name, query_json)?;
        let id = CollectionId::new_v7();
        self.connection.execute(
            "INSERT INTO library_albums(id, kind, name, query_json, created_at_ms, updated_at_ms)
             VALUES (?1, ?2, ?3, ?4, ?5, ?5)",
            params![
                id.as_bytes().as_slice(),
                kind.as_str(),
                name,
                query_json.as_deref(),
                now_ms,
            ],
        )?;
        Ok(AlbumRecord {
            id,
            kind,
            name,
            query_json,
            created_at_ms: now_ms,
            updated_at_ms: now_ms,
        })
    }

    /// Creates a smart album from the current, strict v1 query contract.
    pub fn create_smart_library_album(
        &mut self,
        name: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let query_json = query.to_json()?;
        self.create_library_album(AlbumKind::Smart, name, Some(&query_json), now_ms)
    }

    /// Lists albums in stable case-insensitive name order.
    pub fn library_albums(&self) -> Result<Vec<AlbumRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, kind, name, query_json, created_at_ms, updated_at_ms
             FROM library_albums ORDER BY name COLLATE NOCASE, id",
        )?;
        let rows = statement.query_map([], read_album)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Renames one manual or smart album without changing its membership or
    /// query. Smart queries are re-canonicalized while validating the update.
    pub fn rename_library_album(
        &mut self,
        album_id: CollectionId,
        name: &str,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let existing = self.library_album_by_id(album_id)?;
        let (name, query_json) =
            validate_album(existing.kind, name, existing.query_json.as_deref())?;
        self.connection.execute(
            "UPDATE library_albums
             SET name = ?2, query_json = ?3, updated_at_ms = ?4
             WHERE id = ?1",
            params![
                album_id.as_bytes().as_slice(),
                name,
                query_json.as_deref(),
                now_ms,
            ],
        )?;
        Ok(AlbumRecord {
            id: album_id,
            kind: existing.kind,
            name,
            query_json,
            created_at_ms: existing.created_at_ms,
            updated_at_ms: now_ms,
        })
    }

    /// Replaces the query of a smart album. Manual albums cannot acquire a
    /// query later: that keeps explicit membership and computed membership
    /// unambiguous in both the UI and the catalog.
    pub fn replace_smart_album_query(
        &mut self,
        album_id: CollectionId,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        let existing = self.library_album_by_id(album_id)?;
        if existing.kind != AlbumKind::Smart {
            return Err(CatalogError::InvalidAlbum(
                "manual albums cannot be given a smart query".into(),
            ));
        }
        let query_json = query.to_json()?;
        self.connection.execute(
            "UPDATE library_albums SET query_json = ?2, updated_at_ms = ?3 WHERE id = ?1",
            params![album_id.as_bytes().as_slice(), query_json.as_str(), now_ms],
        )?;
        Ok(AlbumRecord {
            id: album_id,
            kind: AlbumKind::Smart,
            name: existing.name,
            query_json: Some(query_json),
            created_at_ms: existing.created_at_ms,
            updated_at_ms: now_ms,
        })
    }

    /// Deletes one album. SQLite cascades only that album's explicit manual
    /// memberships; it never deletes a photo, source, or another album.
    pub fn delete_library_album(&mut self, album_id: CollectionId) -> Result<bool, CatalogError> {
        let deleted = self.connection.execute(
            "DELETE FROM library_albums WHERE id = ?1",
            [album_id.as_bytes().as_slice()],
        )?;
        Ok(deleted != 0)
    }

    /// Resolves the query behind one smart album. Manual albums deliberately
    /// remain membership-backed and must be queried with `album_id` instead.
    pub fn smart_album_filter(
        &self,
        album_id: CollectionId,
    ) -> Result<LibraryPhotoFilter, CatalogError> {
        let album = self.library_album_by_id(album_id)?;
        match (album.kind, album.query_json.as_deref()) {
            (AlbumKind::Smart, Some(query_json)) => {
                SmartAlbumQueryV1::from_json(query_json)?.library_filter()
            }
            (AlbumKind::Smart, None) => Err(CatalogError::InvalidAlbum(
                "persisted smart album is missing its query".into(),
            )),
            (AlbumKind::Manual, _) => Err(CatalogError::InvalidAlbum(
                "manual albums do not have a smart query".into(),
            )),
        }
    }

    fn library_album_by_id(&self, album_id: CollectionId) -> Result<AlbumRecord, CatalogError> {
        self.connection
            .query_row(
                "SELECT id, kind, name, query_json, created_at_ms, updated_at_ms
                 FROM library_albums WHERE id = ?1",
                [album_id.as_bytes().as_slice()],
                read_album,
            )
            .optional()?
            .ok_or(CatalogError::AlbumNotFound(album_id))
    }

    /// Pages one smart album through the normal keyset grid implementation.
    pub fn smart_album_photo_page(
        &self,
        album_id: CollectionId,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        let filter = self.smart_album_filter(album_id)?;
        self.library_photo_page(&filter, after, requested_limit)
    }

    /// Counts one settled smart album through the normal indexed filter path.
    pub fn smart_album_photo_count(&self, album_id: CollectionId) -> Result<u64, CatalogError> {
        let filter = self.smart_album_filter(album_id)?;
        self.library_photo_count(&filter)
    }

    /// Adds a photo to a manual album. Repeating the same operation is
    /// idempotent and preserves its original order.
    pub fn add_photo_to_album(
        &mut self,
        album_id: CollectionId,
        photo_id: PhotoId,
        sort_key: i64,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        ensure_photo_exists(&transaction, photo_id)?;
        ensure_manual_album(&transaction, album_id)?;
        transaction.execute(
            "INSERT INTO library_album_memberships(album_id, photo_id, added_at_ms, sort_key)
             VALUES (?1, ?2, ?3, ?4) ON CONFLICT(album_id, photo_id) DO NOTHING",
            params![
                album_id.as_bytes().as_slice(),
                photo_id.as_bytes().as_slice(),
                now_ms,
                sort_key,
            ],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Removes the membership from one album only; it never removes the photo
    /// from the Library or any other album.
    pub fn remove_photo_from_album(
        &mut self,
        album_id: CollectionId,
        photo_id: PhotoId,
    ) -> Result<bool, CatalogError> {
        let removed = self.connection.execute(
            "DELETE FROM library_album_memberships WHERE album_id = ?1 AND photo_id = ?2",
            params![
                album_id.as_bytes().as_slice(),
                photo_id.as_bytes().as_slice()
            ],
        )?;
        Ok(removed != 0)
    }

    /// Lists the explicit manual-album memberships for one photo.
    ///
    /// Smart albums are computed from their query at browse time, so they do
    /// not create mutable membership rows and are intentionally absent here.
    pub fn albums_for_photo(&self, photo_id: PhotoId) -> Result<Vec<AlbumRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT a.id, a.kind, a.name, a.query_json, a.created_at_ms, a.updated_at_ms
             FROM library_albums a
             JOIN library_album_memberships m ON m.album_id = a.id
             WHERE m.photo_id = ?1
             ORDER BY a.name COLLATE NOCASE, a.id",
        )?;
        let rows = statement.query_map([photo_id.as_bytes().as_slice()], read_album)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Lists configured scan sources. Source records are durable even if all
    /// their current locations become unavailable.
    pub fn library_sources(&self) -> Result<Vec<LibrarySourceRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, platform, native_path, display_path, enabled, created_at_ms, last_scanned_at_ms
             FROM library_sources ORDER BY enabled DESC, display_path COLLATE NOCASE, id",
        )?;
        let rows = statement.query_map([], read_library_source)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
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
             ORDER BY s.enabled DESC, s.display_path COLLATE NOCASE, s.id",
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
        let next_cursor = has_more.then(|| {
            let last = items
                .last()
                .expect("a page with a successor contains one item");
            MissingSourceLocationCursor {
                location_id: last.location_id,
            }
        });
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
}

pub(crate) fn upsert_photo_library_facts_in_transaction(
    transaction: &Transaction<'_>,
    facts: &LibraryPhotoFacts,
) -> Result<(), CatalogError> {
    validate_facts(facts)?;
    ensure_photo_exists(transaction, facts.photo_id)?;
    if let Some(representation_id) = facts.indexed_representation_id {
        ensure_representation_belongs_to_photo(transaction, representation_id, facts.photo_id)?;
    }
    let source_byte_len = facts
        .indexed_source
        .map(|source| i64::try_from(source.byte_len))
        .transpose()
        .map_err(|error| CatalogError::InvalidLibraryFacts(error.to_string()))?;
    let indexed_representation_bytes = facts
        .indexed_representation_id
        .map(|value| value.as_bytes().to_vec());
    transaction.execute(
        "INSERT INTO photo_library_facts(
             photo_id, captured_at_unix_seconds, capture_day,
             camera_make, camera_model, camera_key,
             lens_make, lens_model, lens_key,
             aperture_milli, focal_length_tenth_mm, iso_speed,
             latitude_e7, longitude_e7, place_name,
             indexed_representation_id, indexed_source_byte_len,
             indexed_source_modified_at_ms, indexed_at_ms
         ) VALUES (
             ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9,
             ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19
         ) ON CONFLICT(photo_id) DO UPDATE SET
             captured_at_unix_seconds = excluded.captured_at_unix_seconds,
             capture_day = excluded.capture_day,
             camera_make = excluded.camera_make,
             camera_model = excluded.camera_model,
             camera_key = excluded.camera_key,
             lens_make = excluded.lens_make,
             lens_model = excluded.lens_model,
             lens_key = excluded.lens_key,
             aperture_milli = excluded.aperture_milli,
             focal_length_tenth_mm = excluded.focal_length_tenth_mm,
             iso_speed = excluded.iso_speed,
             latitude_e7 = excluded.latitude_e7,
             longitude_e7 = excluded.longitude_e7,
             place_name = excluded.place_name,
             indexed_representation_id = excluded.indexed_representation_id,
             indexed_source_byte_len = excluded.indexed_source_byte_len,
             indexed_source_modified_at_ms = excluded.indexed_source_modified_at_ms,
             indexed_at_ms = excluded.indexed_at_ms",
        params![
            facts.photo_id.as_bytes().as_slice(),
            facts.captured_at_unix_seconds,
            facts.capture_day.trim(),
            facts.camera_make.trim(),
            facts.camera_model.trim(),
            normalized_equipment_key(&facts.camera_make, &facts.camera_model),
            facts.lens_make.trim(),
            facts.lens_model.trim(),
            normalized_equipment_key(&facts.lens_make, &facts.lens_model),
            facts.aperture_milli.map(i64::from),
            facts.focal_length_tenth_mm.map(i64::from),
            facts.iso_speed,
            facts.latitude_e7.map(i64::from),
            facts.longitude_e7.map(i64::from),
            facts.place_name.trim(),
            indexed_representation_bytes.as_deref(),
            source_byte_len,
            facts
                .indexed_source
                .and_then(|source| source.modified_at_ms),
            facts.indexed_at_ms,
        ],
    )?;
    Ok(())
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
             id, representation_id, platform, native_path, display_path, status, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, 'online', ?6)",
        params![
            location_id.as_bytes().as_slice(),
            existing.representation_id.as_bytes().as_slice(),
            request.location.platform.as_str(),
            request.location.native_path.as_slice(),
            request.location.display_path,
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

fn ensure_photo_exists(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let exists: Option<i64> = transaction
        .query_row(
            "SELECT 1 FROM photos WHERE id = ?1",
            [photo_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?;
    if exists.is_none() {
        return Err(CatalogError::PhotoNotFound(photo_id));
    }
    Ok(())
}

fn ensure_photo_exists_connection(
    connection: &rusqlite::Connection,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let exists: Option<i64> = connection
        .query_row(
            "SELECT 1 FROM photos WHERE id = ?1",
            [photo_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?;
    if exists.is_none() {
        return Err(CatalogError::PhotoNotFound(photo_id));
    }
    Ok(())
}

fn ensure_representation_belongs_to_photo(
    transaction: &Transaction<'_>,
    representation_id: RepresentationId,
    photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let owner: Option<PhotoId> = transaction
        .query_row(
            "SELECT photo_id FROM representations WHERE id = ?1",
            [representation_id.as_bytes().as_slice()],
            |row| read_id(row, 0),
        )
        .optional()?;
    match owner {
        Some(owner) if owner == photo_id => Ok(()),
        Some(_) => Err(CatalogError::InvalidLibraryFacts(
            "indexed representation belongs to another photo".into(),
        )),
        None => Err(CatalogError::RepresentationNotFound(representation_id)),
    }
}

fn ensure_manual_album(
    transaction: &Transaction<'_>,
    album_id: CollectionId,
) -> Result<(), CatalogError> {
    let kind: Option<String> = transaction
        .query_row(
            "SELECT kind FROM library_albums WHERE id = ?1",
            [album_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?;
    match kind.as_deref() {
        Some("manual") => Ok(()),
        Some("smart") => Err(CatalogError::InvalidAlbum(
            "photos cannot be manually inserted into a smart album".into(),
        )),
        Some(other) => Err(CatalogError::InvalidAlbum(format!(
            "unknown persisted album kind {other:?}"
        ))),
        None => Err(CatalogError::AlbumNotFound(album_id)),
    }
}

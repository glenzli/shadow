//! Photo-first Library projections, collections, and path-independent source identity.
//!
//! The catalog's foundational `photos -> representations -> locations` graph remains the
//! authoritative model. This module adds the durable projections needed to browse that graph at
//! scale without making an import directory the owner of a photo. All cache payloads remain in
//! `shadow-cache`; SQLite stores only small, indexed facts and references.

use rusqlite::{
    OptionalExtension, Transaction, params, params_from_iter,
    types::{Type, Value},
};
use serde::{Deserialize, Serialize};
use shadow_domain::{
    AssetLocation, CollectionId, EntityId, ImportSessionId, LibrarySourceId, LocationId,
    PhotoDecisionState, PhotoFlag, PhotoId, RepresentationId, RepresentationKind,
};
use uuid::Uuid;

use crate::{
    Catalog, CatalogError, RegisterAsset, RegisteredAsset, RepresentationFingerprint,
    SourceScanReconciliation, decision::photo_decision_state_from_columns,
    decode_snapshot::representation_fingerprint_in_transaction, find_existing_asset, insert_asset,
    read_id,
};

/// The largest page the catalog will materialize for one Library request.
///
/// Keeping this bounded is important even when the UI virtualizes its grid:
/// a large library must never turn one scroll event into an unbounded SQLite
/// allocation.
pub const MAX_LIBRARY_PAGE_SIZE: usize = 512;

/// The largest number of one Library facet's values materialized by one
/// request. Facets are discovery aids, not an invitation to load every lens
/// or every month in a multi-million-photo catalog into the desktop shell.
pub const MAX_LIBRARY_FACET_PAGE_SIZE: usize = 48;

/// The semantic domain represented by a source identity digest.
///
/// `WholeFile` is the only generic exact identity today. `FormatPayload` and
/// `DecodedMosaic` are intentionally first-class for decoder providers: they
/// can survive a metadata-only rewrite without pretending that every RAW
/// format exposes a universal payload byte range.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ContentIdentityScope {
    WholeFile,
    FormatPayload,
    DecodedMosaic,
}

impl ContentIdentityScope {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::WholeFile => "whole_file",
            Self::FormatPayload => "format_payload",
            Self::DecodedMosaic => "decoded_mosaic",
        }
    }
}

/// A versioned, path-independent content identity.
///
/// `provider_id` and `provider_version` must be empty for a whole-file hash.
/// Provider-produced raw payload or mosaic identities must include both so
/// future decoder changes cannot silently claim byte-for-byte compatibility.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ContentIdentity {
    pub scope: ContentIdentityScope,
    pub algorithm: String,
    pub provider_id: String,
    pub provider_version: String,
    pub digest: [u8; 32],
}

impl ContentIdentity {
    #[must_use]
    pub fn whole_file_blake3(digest: [u8; 32]) -> Self {
        Self {
            scope: ContentIdentityScope::WholeFile,
            algorithm: "blake3-256".to_owned(),
            provider_id: String::new(),
            provider_version: String::new(),
            digest,
        }
    }

    pub(crate) fn validate(&self) -> Result<(), CatalogError> {
        let valid_text = |value: &str, maximum: usize| {
            !value.is_empty()
                && value.len() <= maximum
                && value
                    .bytes()
                    .all(|byte| byte.is_ascii_graphic() || byte == b' ')
        };
        if !valid_text(&self.algorithm, 128) {
            return Err(CatalogError::InvalidContentIdentity(
                "algorithm must be nonempty printable text up to 128 bytes".into(),
            ));
        }
        if self.provider_id.len() > 128 || self.provider_version.len() > 128 {
            return Err(CatalogError::InvalidContentIdentity(
                "provider id and version must not exceed 128 bytes".into(),
            ));
        }
        if self.scope == ContentIdentityScope::WholeFile
            && (!self.provider_id.is_empty() || !self.provider_version.is_empty())
        {
            return Err(CatalogError::InvalidContentIdentity(
                "whole-file identities must not name a decoder provider".into(),
            ));
        }
        if self.scope != ContentIdentityScope::WholeFile
            && (self.provider_id.is_empty() || self.provider_version.is_empty())
        {
            return Err(CatalogError::InvalidContentIdentity(
                "payload and mosaic identities require provider id and version".into(),
            ));
        }
        Ok(())
    }
}

/// A content identity whose digest was calculated for one observed source
/// revision.
///
/// Background hashing is intentionally outside the catalog writer. The
/// expected fingerprint makes its eventual write a compare-and-swap: a late
/// result never becomes an identity for a newer file at the same path.
#[derive(Debug, Clone, PartialEq)]
pub struct RecordRepresentationContentIdentity {
    pub representation_id: RepresentationId,
    pub expected_source: RepresentationFingerprint,
    pub identity: ContentIdentity,
    pub observed_at_ms: i64,
}

/// Result of conditionally recording a representation content identity.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RecordRepresentationContentIdentityStatus {
    Recorded,
    StaleSource,
}

/// The exact existing representation selected by a path-independent identity.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RelinkMatch {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
}

/// A scan-scoped historical location that a user explicitly selected for an
/// exact source reattach. It is deliberately obtained from the completed scan
/// evidence rather than supplied by the UI, so the expected representation is
/// a catalog fact, never a caller-controlled identifier.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct MissingSourceRelinkTarget {
    pub location: MissingSourceLocationRecord,
}

/// A discovery entry point. It only determines where scans start; a photo can
/// have locations from any number of sources and does not disappear if one is
/// removed.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibrarySourceRecord {
    pub id: LibrarySourceId,
    pub root: AssetLocation,
    pub enabled: bool,
    pub created_at_ms: i64,
    pub last_scanned_at_ms: Option<i64>,
}

/// The latest completed scan evidence for one configured Library source.
///
/// A nonzero `not_seen_locations` count means only that those locations were
/// not registered by this particular completed scan. It is deliberately not a
/// global offline verdict: sources may overlap, a drive may be temporarily
/// unavailable, and the same representation can remain reachable elsewhere.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibrarySourceHealth {
    pub source: LibrarySourceRecord,
    pub latest_completed_scan: Option<SourceScanReconciliation>,
}

/// Stable keyset cursor for source-specific locations absent from one
/// completed scan. It deliberately uses the immutable location id rather
/// than `last_seen_at_ms`: a later scan can update the latter while a review
/// of an earlier session is still open.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct MissingSourceLocationCursor {
    pub location_id: LocationId,
}

/// A location that belongs to a configured source but was not observed in one
/// completed scan of that source.
///
/// This is review/relink input, not a claim that the path is globally absent.
/// The metadata fields are intentionally only weak matching evidence; a
/// content hash is still required before attaching a moved file.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct MissingSourceLocationRecord {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location_id: LocationId,
    pub kind: RepresentationKind,
    pub location: AssetLocation,
    pub source: RepresentationFingerprint,
    pub captured_at_unix_seconds: Option<i64>,
    pub camera_key: String,
    pub last_seen_at_ms: i64,
}

/// One bounded page of locations that were absent from a single completed
/// source scan.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct MissingSourceLocationPage {
    pub reconciliation: SourceScanReconciliation,
    pub items: Vec<MissingSourceLocationRecord>,
    pub next_cursor: Option<MissingSourceLocationCursor>,
}

/// Indexed metadata used by the hot Library filter path. Low-frequency EXIF
/// continues to live in the decoder snapshot JSON rather than an EAV table.
#[derive(Debug, Clone, PartialEq)]
pub struct LibraryPhotoFacts {
    pub photo_id: PhotoId,
    pub captured_at_unix_seconds: Option<i64>,
    /// An ISO local-date string (`YYYY-MM-DD`) supplied by the metadata layer.
    pub capture_day: String,
    pub camera_make: String,
    pub camera_model: String,
    pub lens_make: String,
    pub lens_model: String,
    pub aperture_milli: Option<u32>,
    pub focal_length_tenth_mm: Option<u32>,
    pub iso_speed: Option<f64>,
    pub latitude_e7: Option<i32>,
    pub longitude_e7: Option<i32>,
    pub place_name: String,
    pub indexed_representation_id: Option<RepresentationId>,
    pub indexed_source: Option<RepresentationFingerprint>,
    pub indexed_at_ms: i64,
}

/// Inclusive capture-time bounds in Unix seconds. An absent bound leaves that
/// side of the range open.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LibraryDateRange {
    pub start_inclusive: Option<i64>,
    pub end_inclusive: Option<i64>,
}

/// Inclusive aperture bounds expressed as f-number × 1000, matching the
/// indexed `aperture_milli` representation.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LibraryApertureRange {
    pub minimum_milli: Option<u32>,
    pub maximum_milli: Option<u32>,
}

/// Photo-first Library filter. All conditions are conjunctive; UI-specific
/// facets can compose this value without making a directory part of identity.
///
/// `camera_key` and `lens_key` use the normalized key stored in
/// [`LibraryPhotoFacts`]. Use [`library_equipment_key`] when constructing a
/// key from a make/model pair.
#[derive(Debug, Clone, Eq, PartialEq, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LibraryPhotoFilter {
    pub capture_time: Option<LibraryDateRange>,
    /// A canonical UTC month (`YYYY-MM`) selected from the Library timeline.
    /// It intentionally composes with an optional raw capture-time range so a
    /// saved Smart Album can keep both a broad programmatic range and a human
    /// calendar bucket without inventing a second date representation.
    pub capture_month: Option<String>,
    pub camera_key: Option<String>,
    pub lens_key: Option<String>,
    pub aperture: Option<LibraryApertureRange>,
    pub liked: Option<bool>,
    pub color_label: Option<String>,
    pub flag: Option<PhotoFlag>,
    /// Matches ratings greater than or equal to this value, as photographers
    /// normally expect from a star filter.
    pub minimum_rating: Option<u8>,
    /// Filters on the existence of the photo's current working Recipe ref.
    /// This is independent of preview-cache readiness: a photo remains edited
    /// while a fresh rendered thumbnail is pending.
    pub has_development_edits: Option<bool>,
    /// Restricts the page to one manual album membership.
    pub album_id: Option<CollectionId>,
}

/// The first persisted smart-album contract.
///
/// A smart album is deliberately just a named, typed snapshot of the existing
/// photo-first Library facets. Every populated field is conjunctive, exactly
/// like [`LibraryPhotoFilter`]. Album membership is intentionally excluded:
/// recursive or membership-derived smart albums would make both correctness
/// and million-photo query costs needlessly surprising.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SmartAlbumQueryV1 {
    schema_version: u32,
    #[serde(default)]
    filter: LibraryPhotoFilter,
}

impl SmartAlbumQueryV1 {
    pub const SCHEMA_VERSION: u32 = 1;

    /// Builds a query that can be stored by a smart album.
    pub fn new(filter: LibraryPhotoFilter) -> Result<Self, CatalogError> {
        let query = Self {
            schema_version: Self::SCHEMA_VERSION,
            filter,
        };
        query.validate()?;
        Ok(query)
    }

    /// Decodes and validates a stored smart-album query.
    pub fn from_json(query_json: &str) -> Result<Self, CatalogError> {
        let query = serde_json::from_str::<Self>(query_json).map_err(|error| {
            CatalogError::InvalidAlbum(format!("smart album query is not valid v1 JSON: {error}"))
        })?;
        query.validate()?;
        Ok(query)
    }

    /// Returns a deterministic JSON representation suitable for the catalog.
    pub fn to_json(&self) -> Result<String, CatalogError> {
        self.validate()?;
        serde_json::to_string(self).map_err(|error| {
            CatalogError::InvalidAlbum(format!("could not serialize smart album query: {error}"))
        })
    }

    #[must_use]
    pub const fn schema_version(&self) -> u32 {
        self.schema_version
    }

    /// Returns the executable photo-first Library filter.
    pub fn library_filter(&self) -> Result<LibraryPhotoFilter, CatalogError> {
        self.validate()?;
        Ok(self.filter.clone())
    }

    fn validate(&self) -> Result<(), CatalogError> {
        if self.schema_version != Self::SCHEMA_VERSION {
            return Err(CatalogError::InvalidAlbum(format!(
                "smart album query schema must be v{}, found v{}",
                Self::SCHEMA_VERSION,
                self.schema_version
            )));
        }
        if self.filter.album_id.is_some() {
            return Err(CatalogError::InvalidAlbum(
                "smart album queries cannot depend on album membership".into(),
            ));
        }
        validate_library_photo_filter(&self.filter).map_err(|error| {
            CatalogError::InvalidAlbum(format!("invalid smart album filter: {error}"))
        })
    }
}

/// Stable cursor for capture-time descending Library pages. Photos without
/// indexed capture time deliberately sort after timestamped photos, then by
/// photo id, so partially indexed libraries remain complete and deterministic.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryPhotoCursor {
    pub captured_at_unix_seconds: Option<i64>,
    pub photo_id: PhotoId,
}

/// One logical photo in the Library grid. `location` is the most recently
/// seen online original RAW location, not an ownership relationship.
#[derive(Debug, Clone, PartialEq)]
pub struct LibraryPhotoRecord {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location: AssetLocation,
    pub source: RepresentationFingerprint,
    pub facts: Option<LibraryPhotoFacts>,
    pub state: PhotoLibraryState,
    pub decision: PhotoDecisionState,
    /// Whether a durable working development recipe exists for this photo.
    /// It is projected with the hot row, so the grid never scans cached
    /// previews or recipe JSON merely to implement its edited filter.
    pub has_development_edits: bool,
}

/// A bounded Library page plus a stable cursor for the next request.
///
/// Deliberately does not carry an exact total. Counting a highly filtered
/// 10-million-photo Library on every scroll is the wrong performance model;
/// callers can request an explicit count after a filter settles.
#[derive(Debug, Clone, PartialEq)]
pub struct LibraryPhotoPage {
    pub items: Vec<LibraryPhotoRecord>,
    pub next_cursor: Option<LibraryPhotoCursor>,
}

/// One dimension that can be grouped from the indexed, photo-first Library
/// projection. These dimensions never expose source folders as an ownership
/// hierarchy: a folder remains only a discovery root.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum LibraryFacetKind {
    CaptureMonth,
    Camera,
    Lens,
}

/// Stable continuation for a descending-by-use facet page. The key breaks
/// ties so a lens or camera cannot disappear between page requests merely
/// because another facet has the same photo count.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryFacetCursor {
    pub photo_count: u64,
    pub key: String,
}

/// A compact, immediately displayable facet row. `key` is the exact typed
/// filter value; `label` is the preserved human-facing metadata text.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryFacetValue {
    pub key: String,
    pub label: String,
    pub photo_count: u64,
}

/// A bounded page of one facet's most-used values. The Catalog only evaluates
/// this explicitly requested aggregate in a worker; ordinary grid scrolling
/// continues to use [`LibraryPhotoPage`] keyset pagination.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryFacetPage {
    pub items: Vec<LibraryFacetValue>,
    pub next_cursor: Option<LibraryFacetCursor>,
}

/// Builds the normalized identity key used by the camera and lens facets.
#[must_use]
pub fn library_equipment_key(make: &str, model: &str) -> String {
    normalized_equipment_key(make, model)
}

/// Current, photo-level Library state. It deliberately does not overload
/// picked/rejected or star rating: a heart is a fast personal affinity signal.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SetPhotoLibraryState {
    pub photo_id: PhotoId,
    pub liked: bool,
    pub color_label: String,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PhotoLibraryState {
    pub photo_id: PhotoId,
    pub liked: bool,
    pub color_label: String,
    pub updated_at_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum AlbumKind {
    Manual,
    Smart,
}

impl AlbumKind {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Manual => "manual",
            Self::Smart => "smart",
        }
    }

    fn parse(value: &str) -> Result<Self, CatalogError> {
        match value {
            "manual" => Ok(Self::Manual),
            "smart" => Ok(Self::Smart),
            _ => Err(CatalogError::InvalidAlbum(format!(
                "unknown album kind {value:?}"
            ))),
        }
    }
}

/// A manual album or a stored smart-album query. The first Library UI only
/// writes manual albums, while the schema leaves a stable home for the query
/// AST before that UI ships.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct AlbumRecord {
    pub id: CollectionId,
    pub kind: AlbumKind,
    pub name: String,
    pub query_json: Option<String>,
    pub created_at_ms: i64,
    pub updated_at_ms: i64,
}

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

    /// Returns one bounded, photo-first Library grid page.
    ///
    /// The query deliberately starts with logical photos rather than imported
    /// folders. It selects an online original source representation (RAW or
    /// raster) and location only as an opening target, while metadata, likes,
    /// decisions, and album membership stay attached to the logical photo.
    /// When a logical photo has both kinds, RAW remains preferred so existing
    /// edit/development flows keep their source choice.
    ///
    /// Pagination is keyset-based: an ordinary scroll does not become slower
    /// as a catalog grows from thousands to millions of images.
    pub fn library_photo_page(
        &self,
        filter: &LibraryPhotoFilter,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        validate_library_photo_filter(filter)?;
        let page_size = requested_limit.clamp(1, MAX_LIBRARY_PAGE_SIZE);
        let (from_sql, where_sql, filter_values) = library_photo_query_parts(filter);

        let mut page_sql = format!(
            "SELECT p.id, r.id, l.platform, l.native_path, l.display_path,
                    r.byte_len, r.modified_at_ms,
                    f.photo_id, f.captured_at_unix_seconds, f.capture_day,
                    f.camera_make, f.camera_model, f.lens_make, f.lens_model,
                    f.aperture_milli, f.focal_length_tenth_mm, f.iso_speed,
                    f.latitude_e7, f.longitude_e7, f.place_name,
                    f.indexed_representation_id, f.indexed_source_byte_len,
                    f.indexed_source_modified_at_ms, f.indexed_at_ms,
                    COALESCE(s.liked, 0), COALESCE(s.color_label, 'none'),
                    COALESCE(s.updated_at_ms, 0),
                    dc.head_sequence, de.after_flag, de.after_rating,
                    EXISTS (
                        SELECT 1 FROM recipe_refs edit_ref
                        WHERE edit_ref.photo_id = p.id
                          AND edit_ref.name = 'working'
                    )
             {from_sql} WHERE {where_sql}"
        );
        let mut page_values = filter_values;
        if let Some(cursor) = after {
            match cursor.captured_at_unix_seconds {
                Some(captured_at) => {
                    page_sql.push_str(
                        " AND (f.captured_at_unix_seconds IS NULL
                              OR f.captured_at_unix_seconds < ?
                              OR (f.captured_at_unix_seconds = ? AND p.id < ?))",
                    );
                    page_values.push(Value::Integer(captured_at));
                    page_values.push(Value::Integer(captured_at));
                    page_values.push(Value::Blob(cursor.photo_id.as_bytes().to_vec()));
                }
                None => {
                    page_sql.push_str(" AND f.captured_at_unix_seconds IS NULL AND p.id < ?");
                    page_values.push(Value::Blob(cursor.photo_id.as_bytes().to_vec()));
                }
            }
        }
        page_sql.push_str(
            " ORDER BY CASE WHEN f.captured_at_unix_seconds IS NULL THEN 1 ELSE 0 END,
                       f.captured_at_unix_seconds DESC, p.id DESC
              LIMIT ?",
        );
        page_values.push(Value::Integer(
            i64::try_from(page_size + 1).unwrap_or(i64::MAX),
        ));

        let mut statement = self.connection.prepare(&page_sql)?;
        let rows = statement.query_map(params_from_iter(page_values.iter()), read_library_photo)?;
        let mut items = rows.collect::<rusqlite::Result<Vec<_>>>()?;
        let has_more = items.len() > page_size;
        items.truncate(page_size);
        let next_cursor = has_more.then(|| {
            let last = items
                .last()
                .expect("page has an item when it has a successor");
            LibraryPhotoCursor {
                captured_at_unix_seconds: last
                    .facts
                    .as_ref()
                    .and_then(|facts| facts.captured_at_unix_seconds),
                photo_id: last.photo_id,
            }
        });
        Ok(LibraryPhotoPage { items, next_cursor })
    }

    /// Calculates an exact photo count for a settled Library filter.
    ///
    /// This is intentionally separate from [`Self::library_photo_page`]. A
    /// virtualized grid should fetch keyset pages immediately and schedule this
    /// potentially expensive aggregate only after the user stops changing
    /// facets.
    pub fn library_photo_count(&self, filter: &LibraryPhotoFilter) -> Result<u64, CatalogError> {
        validate_library_photo_filter(filter)?;
        let (from_sql, where_sql, values) = library_photo_query_parts(filter);
        let total: i64 = self.connection.query_row(
            &format!("SELECT COUNT(*) {from_sql} WHERE {where_sql}"),
            params_from_iter(values.iter()),
            |row| row.get(0),
        )?;
        u64::try_from(total).map_err(|error| CatalogError::InvalidLibraryQuery(error.to_string()))
    }

    /// Returns a bounded page of one dynamically composed Library facet.
    ///
    /// The selected dimension is intentionally removed from the incoming
    /// filter before grouping. This is the useful Lightroom-style behaviour:
    /// after selecting one camera, the Camera section still shows meaningful
    /// alternatives while all other active constraints remain in force.
    ///
    /// Facets are ordered by matching photo count, then a stable key. The
    /// aggregate runs only when the desktop explicitly opens or refreshes the
    /// facet browser; it is not part of the hot scroll path.
    pub fn library_facet_page(
        &self,
        filter: &LibraryPhotoFilter,
        kind: LibraryFacetKind,
        after: Option<&LibraryFacetCursor>,
        requested_limit: usize,
    ) -> Result<LibraryFacetPage, CatalogError> {
        validate_library_photo_filter(filter)?;
        if let Some(cursor) = after
            && (cursor.key.trim().is_empty() || cursor.key.len() > 512)
        {
            return Err(CatalogError::InvalidLibraryQuery(
                "facet cursor key must contain 1 through 512 characters".into(),
            ));
        }

        let page_size = requested_limit.clamp(1, MAX_LIBRARY_FACET_PAGE_SIZE);
        let facet_filter = filter_without_facet(filter, kind);
        let (from_sql, where_sql, mut values) = library_photo_query_parts(&facet_filter);
        let spec = library_facet_sql(kind);

        let mut sql = format!(
            "SELECT {key_sql}, {label_sql}, COUNT(*)\n             {from_sql}\n             WHERE {where_sql} AND {present_sql}\n             GROUP BY {key_sql}",
            key_sql = spec.key_sql,
            label_sql = spec.label_sql,
            present_sql = spec.present_sql,
        );
        if let Some(cursor) = after {
            sql.push_str(&format!(
                " HAVING COUNT(*) < ? OR (COUNT(*) = ? AND {key_sql} > ?)",
                key_sql = spec.key_sql,
            ));
            let count = i64::try_from(cursor.photo_count).map_err(|error| {
                CatalogError::InvalidLibraryQuery(format!("facet cursor count is invalid: {error}"))
            })?;
            values.push(Value::Integer(count));
            values.push(Value::Integer(count));
            values.push(Value::Text(cursor.key.clone()));
        }
        sql.push_str(&format!(
            " ORDER BY COUNT(*) DESC, {key_sql} ASC LIMIT ?",
            key_sql = spec.key_sql,
        ));
        values.push(Value::Integer(
            i64::try_from(page_size + 1).unwrap_or(i64::MAX),
        ));

        let mut statement = self.connection.prepare(&sql)?;
        let rows = statement.query_map(params_from_iter(values.iter()), read_library_facet)?;
        let mut items = rows.collect::<rusqlite::Result<Vec<_>>>()?;
        let has_more = items.len() > page_size;
        items.truncate(page_size);
        let next_cursor = has_more.then(|| {
            let last = items
                .last()
                .expect("a facet page with a successor contains one item");
            LibraryFacetCursor {
                photo_count: last.photo_count,
                key: last.key.clone(),
            }
        });
        Ok(LibraryFacetPage { items, next_cursor })
    }
}

struct LibraryFacetSql {
    key_sql: &'static str,
    label_sql: &'static str,
    present_sql: &'static str,
}

fn library_facet_sql(kind: LibraryFacetKind) -> LibraryFacetSql {
    match kind {
        LibraryFacetKind::CaptureMonth => LibraryFacetSql {
            key_sql: "substr(f.capture_day, 1, 7)",
            label_sql: "substr(f.capture_day, 1, 7)",
            present_sql: "f.capture_day <> ''",
        },
        LibraryFacetKind::Camera => LibraryFacetSql {
            key_sql: "f.camera_key",
            label_sql: "MIN(COALESCE(NULLIF(trim(f.camera_make || ' ' || f.camera_model), ''), f.camera_key))",
            present_sql: "f.camera_key <> ''",
        },
        LibraryFacetKind::Lens => LibraryFacetSql {
            key_sql: "f.lens_key",
            label_sql: "MIN(COALESCE(NULLIF(trim(f.lens_make || ' ' || f.lens_model), ''), f.lens_key))",
            present_sql: "f.lens_key <> ''",
        },
    }
}

fn filter_without_facet(
    filter: &LibraryPhotoFilter,
    kind: LibraryFacetKind,
) -> LibraryPhotoFilter {
    let mut result = filter.clone();
    match kind {
        LibraryFacetKind::CaptureMonth => result.capture_month = None,
        LibraryFacetKind::Camera => result.camera_key = None,
        LibraryFacetKind::Lens => result.lens_key = None,
    }
    result
}

fn library_photo_query_parts(filter: &LibraryPhotoFilter) -> (String, String, Vec<Value>) {
    // Both correlated subqueries are backed by the current v1 catalog's
    // `(photo, kind, created)` and `(representation, status, created)` indexes.
    // This keeps one logical
    // row per photo without requiring a directory-derived materialized view.
    // Original rasters are first-class Library sources; RAW retains a stable
    // preference only when both are attached to one logical photo.
    let from_sql = "FROM photos p
         JOIN representations r ON r.id = (
             SELECT r2.id FROM representations r2
             WHERE r2.photo_id = p.id
               AND r2.kind IN ('original_raw', 'original_raster')
               AND EXISTS (
                   SELECT 1 FROM locations l2
                   WHERE l2.representation_id = r2.id AND l2.status = 'online'
               )
             ORDER BY CASE r2.kind WHEN 'original_raw' THEN 0 ELSE 1 END,
                      r2.created_at_ms DESC, r2.id DESC
             LIMIT 1
         )
         JOIN locations l ON l.id = (
             SELECT l3.id FROM locations l3
             WHERE l3.representation_id = r.id AND l3.status = 'online'
             ORDER BY l3.created_at_ms DESC, l3.id DESC
             LIMIT 1
         )
         LEFT JOIN photo_library_facts f ON f.photo_id = p.id
         LEFT JOIN photo_library_state s ON s.photo_id = p.id
         LEFT JOIN photo_decision_current dc ON dc.photo_id = p.id
         LEFT JOIN photo_decision_events de
           ON de.sequence = dc.head_sequence AND de.photo_id = p.id"
        .to_owned();
    let mut clauses = vec!["p.lifecycle_state = 'active'".to_owned()];
    let mut values = Vec::new();

    if let Some(range) = filter.capture_time {
        if let Some(start) = range.start_inclusive {
            clauses.push("f.captured_at_unix_seconds >= ?".to_owned());
            values.push(Value::Integer(start));
        }
        if let Some(end) = range.end_inclusive {
            clauses.push("f.captured_at_unix_seconds <= ?".to_owned());
            values.push(Value::Integer(end));
        }
    }
    if let Some(capture_month) = filter.capture_month.as_deref() {
        // Every public query entry validates the filter before reaching this
        // hot SQL-fragment builder, so an invalid month here would be an
        // internal call-order bug rather than user input.
        let (first_day, next_first_day) = capture_month_bounds(capture_month)
            .expect("validated Library capture month must have bounds");
        clauses.push("f.capture_day >= ? AND f.capture_day < ?".to_owned());
        values.push(Value::Text(first_day));
        values.push(Value::Text(next_first_day));
    }
    if let Some(camera_key) = filter.camera_key.as_deref() {
        clauses.push("f.camera_key = ?".to_owned());
        values.push(Value::Text(normalize_query_key(camera_key)));
    }
    if let Some(lens_key) = filter.lens_key.as_deref() {
        clauses.push("f.lens_key = ?".to_owned());
        values.push(Value::Text(normalize_query_key(lens_key)));
    }
    if let Some(range) = filter.aperture {
        if let Some(minimum) = range.minimum_milli {
            clauses.push("f.aperture_milli >= ?".to_owned());
            values.push(Value::Integer(i64::from(minimum)));
        }
        if let Some(maximum) = range.maximum_milli {
            clauses.push("f.aperture_milli <= ?".to_owned());
            values.push(Value::Integer(i64::from(maximum)));
        }
    }
    if let Some(liked) = filter.liked {
        clauses.push("COALESCE(s.liked, 0) = ?".to_owned());
        values.push(Value::Integer(i64::from(liked)));
    }
    if let Some(color_label) = filter.color_label.as_deref() {
        clauses.push("COALESCE(s.color_label, 'none') = ?".to_owned());
        values.push(Value::Text(color_label.trim().to_ascii_lowercase()));
    }
    if let Some(flag) = filter.flag {
        clauses.push("COALESCE(de.after_flag, 'unflagged') = ?".to_owned());
        values.push(Value::Text(flag.as_str().to_owned()));
    }
    if let Some(minimum_rating) = filter.minimum_rating {
        clauses.push("COALESCE(de.after_rating, 0) >= ?".to_owned());
        values.push(Value::Integer(i64::from(minimum_rating)));
    }
    if let Some(has_development_edits) = filter.has_development_edits {
        let exists_working_recipe = "EXISTS (
             SELECT 1 FROM recipe_refs edit_ref
             WHERE edit_ref.photo_id = p.id
               AND edit_ref.name = 'working'
         )";
        clauses.push(if has_development_edits {
            exists_working_recipe.to_owned()
        } else {
            format!("NOT {exists_working_recipe}")
        });
    }
    if let Some(album_id) = filter.album_id {
        clauses.push(
            "EXISTS (
                 SELECT 1 FROM library_album_memberships m
                 WHERE m.photo_id = p.id AND m.album_id = ?
             )"
            .to_owned(),
        );
        values.push(Value::Blob(album_id.as_bytes().to_vec()));
    }
    (from_sql, clauses.join(" AND "), values)
}

fn validate_library_photo_filter(filter: &LibraryPhotoFilter) -> Result<(), CatalogError> {
    if let Some(range) = filter.capture_time
        && let (Some(start), Some(end)) = (range.start_inclusive, range.end_inclusive)
        && start > end
    {
        return Err(CatalogError::InvalidLibraryQuery(
            "capture-time range starts after it ends".into(),
        ));
    }
    if let Some(range) = filter.aperture
        && let (Some(minimum), Some(maximum)) = (range.minimum_milli, range.maximum_milli)
        && minimum > maximum
    {
        return Err(CatalogError::InvalidLibraryQuery(
            "aperture range starts above its maximum".into(),
        ));
    }
    if filter.minimum_rating.is_some_and(|rating| rating > 5) {
        return Err(CatalogError::InvalidLibraryQuery(
            "minimum rating must be in the inclusive 0 through 5 range".into(),
        ));
    }
    if let Some(capture_month) = filter.capture_month.as_deref()
        && capture_month_bounds(capture_month).is_none()
    {
        return Err(CatalogError::InvalidLibraryQuery(
            "capture month must use the canonical YYYY-MM form".into(),
        ));
    }
    for (name, value, maximum) in [
        ("camera key", filter.camera_key.as_deref(), 512),
        ("lens key", filter.lens_key.as_deref(), 512),
        ("color label", filter.color_label.as_deref(), 64),
    ] {
        if let Some(value) = value
            && (value.trim().is_empty() || value.len() > maximum)
        {
            return Err(CatalogError::InvalidLibraryQuery(format!(
                "{name} must contain 1 through {maximum} characters"
            )));
        }
    }
    Ok(())
}

fn capture_month_bounds(capture_month: &str) -> Option<(String, String)> {
    let bytes = capture_month.as_bytes();
    if bytes.len() != 7 || bytes[4] != b'-'
        || !bytes[..4].iter().all(u8::is_ascii_digit)
        || !bytes[5..].iter().all(u8::is_ascii_digit)
    {
        return None;
    }
    let year = capture_month[..4].parse::<u16>().ok()?;
    let month = capture_month[5..].parse::<u8>().ok()?;
    if !(1..=12).contains(&month) {
        return None;
    }
    let (next_year, next_month) = if month == 12 {
        (year.checked_add(1)?, 1)
    } else {
        (year, month + 1)
    };
    Some((
        format!("{year:04}-{month:02}-01"),
        format!("{next_year:04}-{next_month:02}-01"),
    ))
}

fn normalize_query_key(value: &str) -> String {
    value.trim().to_ascii_lowercase()
}

fn read_library_photo(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryPhotoRecord> {
    let photo_id: PhotoId = read_id(row, 0)?;
    let representation_id = read_id(row, 1)?;
    let platform = platform_from_text(&row.get::<_, String>(2)?, 2)?;
    let byte_len: i64 = row.get(5)?;
    let source = RepresentationFingerprint {
        byte_len: u64::try_from(byte_len).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(5, Type::Integer, Box::new(error))
        })?,
        modified_at_ms: row.get(6)?,
    };
    let decision = photo_decision_state_from_columns(row.get(27)?, row.get(28)?, row.get(29)?)
        .map_err(|error| invalid_data(27, error.to_string()))?;
    Ok(LibraryPhotoRecord {
        photo_id,
        representation_id,
        location: AssetLocation::new(platform, row.get(3)?, row.get::<_, String>(4)?),
        source,
        facts: read_library_facts_from_columns(row, 7)?,
        state: PhotoLibraryState {
            photo_id,
            liked: row.get::<_, i64>(24)? != 0,
            color_label: row.get(25)?,
            updated_at_ms: row.get(26)?,
        },
        decision,
        has_development_edits: row.get::<_, i64>(30)? != 0,
    })
}

fn read_library_facet(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryFacetValue> {
    let photo_count: i64 = row.get(2)?;
    Ok(LibraryFacetValue {
        key: row.get(0)?,
        label: row.get(1)?,
        photo_count: u64::try_from(photo_count).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(2, Type::Integer, Box::new(error))
        })?,
    })
}

fn read_library_facts_from_columns(
    row: &rusqlite::Row<'_>,
    start: usize,
) -> rusqlite::Result<Option<LibraryPhotoFacts>> {
    let Some(photo_id) = optional_entity_id::<PhotoId>(row, start)? else {
        return Ok(None);
    };
    let aperture_milli: Option<i64> = row.get(start + 7)?;
    let focal_length_tenth_mm: Option<i64> = row.get(start + 8)?;
    let latitude_e7: Option<i64> = row.get(start + 10)?;
    let longitude_e7: Option<i64> = row.get(start + 11)?;
    let source_byte_len: Option<i64> = row.get(start + 14)?;
    Ok(Some(LibraryPhotoFacts {
        photo_id,
        captured_at_unix_seconds: row.get(start + 1)?,
        capture_day: row.get(start + 2)?,
        camera_make: row.get(start + 3)?,
        camera_model: row.get(start + 4)?,
        lens_make: row.get(start + 5)?,
        lens_model: row.get(start + 6)?,
        aperture_milli: optional_u32(aperture_milli, start + 7)?,
        focal_length_tenth_mm: optional_u32(focal_length_tenth_mm, start + 8)?,
        iso_speed: row.get(start + 9)?,
        latitude_e7: optional_i32(latitude_e7, start + 10)?,
        longitude_e7: optional_i32(longitude_e7, start + 11)?,
        place_name: row.get(start + 12)?,
        indexed_representation_id: optional_entity_id(row, start + 13)?,
        indexed_source: match source_byte_len {
            Some(byte_len) => Some(RepresentationFingerprint {
                byte_len: u64::try_from(byte_len).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        start + 14,
                        Type::Integer,
                        Box::new(error),
                    )
                })?,
                modified_at_ms: row.get(start + 15)?,
            }),
            None => None,
        },
        indexed_at_ms: row.get(start + 16)?,
    }))
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

fn validate_facts(facts: &LibraryPhotoFacts) -> Result<(), CatalogError> {
    if facts.capture_day.len() > 16
        || facts.camera_make.len() > 256
        || facts.camera_model.len() > 256
        || facts.lens_make.len() > 256
        || facts.lens_model.len() > 512
        || facts.place_name.len() > 512
    {
        return Err(CatalogError::InvalidLibraryFacts(
            "a Library metadata text value exceeds its bounded index width".into(),
        ));
    }
    if let Some(iso_speed) = facts.iso_speed
        && (!iso_speed.is_finite() || iso_speed < 0.0)
    {
        return Err(CatalogError::InvalidLibraryFacts(
            "ISO speed must be finite and nonnegative".into(),
        ));
    }
    if let (Some(latitude), Some(longitude)) = (facts.latitude_e7, facts.longitude_e7)
        && (!(-900_000_000..=900_000_000).contains(&latitude)
            || !(-1_800_000_000..=1_800_000_000).contains(&longitude))
    {
        return Err(CatalogError::InvalidLibraryFacts(
            "GPS coordinates are outside the valid latitude/longitude range".into(),
        ));
    }
    if facts.latitude_e7.is_some() != facts.longitude_e7.is_some() {
        return Err(CatalogError::InvalidLibraryFacts(
            "latitude and longitude must be recorded together".into(),
        ));
    }
    Ok(())
}

fn validate_library_state(state: &SetPhotoLibraryState) -> Result<(), CatalogError> {
    let label = state.color_label.trim();
    if label.is_empty()
        || label.len() > 64
        || !label
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
    {
        return Err(CatalogError::InvalidLibraryState(
            "color label must be 1 through 64 ASCII letters, digits, hyphens, or underscores"
                .into(),
        ));
    }
    Ok(())
}

fn validate_album(
    kind: AlbumKind,
    name: &str,
    query_json: Option<&str>,
) -> Result<(String, Option<String>), CatalogError> {
    let name = name.trim();
    if name.is_empty() || name.len() > 256 {
        return Err(CatalogError::InvalidAlbum(
            "album names must contain 1 through 256 characters".into(),
        ));
    }
    match (kind, query_json) {
        (AlbumKind::Manual, None) => Ok((name.to_owned(), None)),
        (AlbumKind::Manual, Some(_)) => Err(CatalogError::InvalidAlbum(
            "manual albums must not carry a smart query".into(),
        )),
        (AlbumKind::Smart, Some(query)) => Ok((
            name.to_owned(),
            Some(SmartAlbumQueryV1::from_json(query)?.to_json()?),
        )),
        (AlbumKind::Smart, _) => Err(CatalogError::InvalidAlbum(
            "smart albums require a valid JSON query".into(),
        )),
    }
}

fn normalized_equipment_key(make: &str, model: &str) -> String {
    [make.trim(), model.trim()]
        .into_iter()
        .filter(|value| !value.is_empty())
        .map(|value| value.to_ascii_lowercase())
        .collect::<Vec<_>>()
        .join("\u{001f}")
}

fn read_library_facts(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryPhotoFacts> {
    let aperture_milli: Option<i64> = row.get(7)?;
    let focal_length_tenth_mm: Option<i64> = row.get(8)?;
    let latitude_e7: Option<i64> = row.get(10)?;
    let longitude_e7: Option<i64> = row.get(11)?;
    let source_byte_len: Option<i64> = row.get(14)?;
    Ok(LibraryPhotoFacts {
        photo_id: read_id(row, 0)?,
        captured_at_unix_seconds: row.get(1)?,
        capture_day: row.get(2)?,
        camera_make: row.get(3)?,
        camera_model: row.get(4)?,
        lens_make: row.get(5)?,
        lens_model: row.get(6)?,
        aperture_milli: optional_u32(aperture_milli, 7)?,
        focal_length_tenth_mm: optional_u32(focal_length_tenth_mm, 8)?,
        iso_speed: row.get(9)?,
        latitude_e7: optional_i32(latitude_e7, 10)?,
        longitude_e7: optional_i32(longitude_e7, 11)?,
        place_name: row.get(12)?,
        indexed_representation_id: optional_entity_id(row, 13)?,
        indexed_source: match source_byte_len {
            Some(byte_len) => Some(RepresentationFingerprint {
                byte_len: u64::try_from(byte_len).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(14, Type::Integer, Box::new(error))
                })?,
                modified_at_ms: row.get(15)?,
            }),
            None => None,
        },
        indexed_at_ms: row.get(16)?,
    })
}

fn read_album(row: &rusqlite::Row<'_>) -> rusqlite::Result<AlbumRecord> {
    let kind: String = row.get(1)?;
    Ok(AlbumRecord {
        id: read_id(row, 0)?,
        kind: AlbumKind::parse(&kind).map_err(|error| invalid_data(1, error.to_string()))?,
        name: row.get(2)?,
        query_json: row.get(3)?,
        created_at_ms: row.get(4)?,
        updated_at_ms: row.get(5)?,
    })
}

fn read_library_source(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibrarySourceRecord> {
    let platform: String = row.get(1)?;
    let platform = platform_from_text(&platform, 1)?;
    Ok(LibrarySourceRecord {
        id: read_id(row, 0)?,
        root: AssetLocation::new(platform, row.get(2)?, row.get::<_, String>(3)?),
        enabled: row.get::<_, i64>(4)? != 0,
        created_at_ms: row.get(5)?,
        last_scanned_at_ms: row.get(6)?,
    })
}

fn read_library_source_health(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibrarySourceHealth> {
    let source = read_library_source(row)?;
    let latest_session_id: Option<ImportSessionId> = optional_entity_id(row, 7)?;
    let latest_completed_scan = latest_session_id
        .map(|session_id| -> rusqlite::Result<SourceScanReconciliation> {
            let known_locations: i64 = row.get(9)?;
            let seen_locations: i64 = row.get(10)?;
            let known_locations = u64::try_from(known_locations).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(9, Type::Integer, Box::new(error))
            })?;
            let seen_locations = u64::try_from(seen_locations).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(10, Type::Integer, Box::new(error))
            })?;
            Ok(SourceScanReconciliation {
                session_id,
                source_id: source.id,
                completed_at_ms: row.get(8)?,
                known_locations,
                seen_locations,
                not_seen_locations: known_locations.saturating_sub(seen_locations),
            })
        })
        .transpose()?;
    Ok(LibrarySourceHealth {
        source,
        latest_completed_scan,
    })
}

fn read_missing_source_location(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<MissingSourceLocationRecord> {
    let platform: String = row.get(3)?;
    let platform = platform_from_text(&platform, 3)?;
    let kind: String = row.get(6)?;
    let byte_len: i64 = row.get(7)?;
    let byte_len = u64::try_from(byte_len).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(7, Type::Integer, Box::new(error))
    })?;
    Ok(MissingSourceLocationRecord {
        photo_id: read_id(row, 0)?,
        representation_id: read_id(row, 1)?,
        location_id: read_id(row, 2)?,
        kind: representation_kind_from_text(&kind, 6)?,
        location: AssetLocation::new(platform, row.get(4)?, row.get::<_, String>(5)?),
        source: RepresentationFingerprint {
            byte_len,
            modified_at_ms: row.get(8)?,
        },
        captured_at_unix_seconds: row.get(9)?,
        camera_key: row.get::<_, Option<String>>(10)?.unwrap_or_default(),
        last_seen_at_ms: row.get(11)?,
    })
}

fn representation_kind_from_text(kind: &str, index: usize) -> rusqlite::Result<RepresentationKind> {
    match kind {
        "original_raw" => Ok(RepresentationKind::OriginalRaw),
        "original_raster" => Ok(RepresentationKind::OriginalRaster),
        "derived_dng" => Ok(RepresentationKind::DerivedDng),
        "embedded_preview" => Ok(RepresentationKind::EmbeddedPreview),
        "scene_linear_rgb" => Ok(RepresentationKind::SceneLinearRgb),
        "vendor_rendered_rgb" => Ok(RepresentationKind::VendorRenderedRgb),
        "proxy" => Ok(RepresentationKind::Proxy),
        _ => Err(invalid_data(
            index,
            format!("unknown persisted representation kind {kind:?}"),
        )),
    }
}

fn platform_from_text(platform: &str, index: usize) -> rusqlite::Result<shadow_domain::Platform> {
    match platform {
        "macos" => Ok(shadow_domain::Platform::MacOs),
        "windows" => Ok(shadow_domain::Platform::Windows),
        "other_unix" => Ok(shadow_domain::Platform::OtherUnix),
        _ => Err(invalid_data(
            index,
            format!("unknown platform {platform:?}"),
        )),
    }
}

fn optional_u32(value: Option<i64>, index: usize) -> rusqlite::Result<Option<u32>> {
    value
        .map(|value| {
            u32::try_from(value).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
            })
        })
        .transpose()
}

fn optional_entity_id<I: EntityId>(
    row: &rusqlite::Row<'_>,
    index: usize,
) -> rusqlite::Result<Option<I>> {
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

fn optional_i32(value: Option<i64>, index: usize) -> rusqlite::Result<Option<i32>> {
    value
        .map(|value| {
            i32::try_from(value).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
            })
        })
        .transpose()
}

fn invalid_data(index: usize, message: String) -> rusqlite::Error {
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
    use crate::{CommitRecipe, ImportSessionState, RecipeRefKind, RecipeRefTarget};
    use shadow_domain::{
        NewPhotoDecisionEvent, PhotoDecisionOrigin, Platform, RecipeCommit, RecipeCommitId,
        RecipeId, RecipeSnapshot, RepresentationKind,
    };

    fn register(catalog: &mut Catalog, path: &str) -> RegisteredAsset {
        register_kind(catalog, path, RepresentationKind::OriginalRaw)
    }

    fn register_kind(
        catalog: &mut Catalog,
        path: &str,
        kind: RepresentationKind,
    ) -> RegisteredAsset {
        catalog
            .register_asset(&RegisterAsset {
                kind,
                location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                byte_len: 100,
                modified_at_ms: Some(10),
                now_ms: 20,
            })
            .expect("register source")
    }

    fn register_scan_entry(
        catalog: &mut Catalog,
        session_id: ImportSessionId,
        path: &str,
        now_ms: i64,
    ) -> RegisteredAsset {
        let request = RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms,
        };
        catalog
            .record_import_discovered(session_id, &request)
            .expect("record scan discovery");
        catalog
            .register_import_asset(session_id, &request)
            .expect("register scan entry")
    }

    #[test]
    fn photo_first_library_page_includes_original_raster_sources() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let raw = register(&mut catalog, "/archive/original.nef");
        let raster = register_kind(
            &mut catalog,
            "/archive/original.jpg",
            RepresentationKind::OriginalRaster,
        );

        let page = catalog
            .library_photo_page(&LibraryPhotoFilter::default(), None, 16)
            .expect("read Library page");
        assert_eq!(
            catalog
                .library_photo_count(&LibraryPhotoFilter::default())
                .expect("count Library photos"),
            2
        );
        assert_eq!(page.items.len(), 2);
        assert!(page.items.iter().any(|item| {
            item.photo_id == raw.photo_id && item.location.display_path == "/archive/original.nef"
        }));
        assert!(page.items.iter().any(|item| {
            item.photo_id == raster.photo_id
                && item.location.display_path == "/archive/original.jpg"
        }));
    }

    #[test]
    fn exact_content_identity_relinks_a_moved_file_without_changing_photo_identity() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original = register(&mut catalog, "/archive/DSC_0001.NEF");
        let identity = ContentIdentity::whole_file_blake3([7; 32]);
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: 100,
                    modified_at_ms: Some(10),
                },
                identity: identity.clone(),
                observed_at_ms: 30,
            })
            .expect("record identity");

        let moved = catalog
            .register_asset_with_content_identity(
                &RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(
                        Platform::MacOs,
                        b"/consolidated/2026/rename.nef".to_vec(),
                        "/consolidated/2026/rename.nef",
                    ),
                    byte_len: 101,
                    modified_at_ms: Some(11),
                    now_ms: 40,
                },
                &identity,
            )
            .expect("relink moved file");

        assert_eq!(moved.photo_id, original.photo_id);
        assert_eq!(moved.representation_id, original.representation_id);
        assert_eq!(moved.status, crate::RegistrationStatus::NeedsRevalidation);
        assert_eq!(catalog.stats().expect("stats").photos, 1);
        assert_eq!(catalog.stats().expect("stats").locations, 2);
        assert_eq!(
            catalog.relink_match(&identity).expect("lookup identity"),
            Some(RelinkMatch {
                photo_id: original.photo_id,
                representation_id: original.representation_id,
            })
        );
    }

    #[test]
    fn source_mutation_invalidates_identity_and_rejects_a_late_hash_result() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original = register(&mut catalog, "/archive/rewritten.nef");
        let identity = ContentIdentity::whole_file_blake3([42; 32]);
        let original_source = RepresentationFingerprint {
            byte_len: 100,
            modified_at_ms: Some(10),
        };
        assert_eq!(
            catalog
                .record_representation_content_identity(&RecordRepresentationContentIdentity {
                    representation_id: original.representation_id,
                    expected_source: original_source,
                    identity: identity.clone(),
                    observed_at_ms: 30,
                })
                .expect("record original identity"),
            RecordRepresentationContentIdentityStatus::Recorded
        );

        let changed = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/archive/rewritten.nef".to_vec(),
                    "/archive/rewritten.nef",
                ),
                byte_len: 101,
                modified_at_ms: Some(11),
                now_ms: 40,
            })
            .expect("observe rewritten source");
        assert_eq!(changed.status, crate::RegistrationStatus::NeedsRevalidation);
        assert_eq!(
            catalog
                .representation_fingerprint(original.representation_id)
                .expect("read current source"),
            RepresentationFingerprint {
                byte_len: 101,
                modified_at_ms: Some(11),
            }
        );
        assert_eq!(
            catalog
                .relink_match(&identity)
                .expect("lookup old identity"),
            None
        );

        assert_eq!(
            catalog
                .record_representation_content_identity(&RecordRepresentationContentIdentity {
                    representation_id: original.representation_id,
                    expected_source: original_source,
                    identity: identity.clone(),
                    observed_at_ms: 41,
                })
                .expect("late write is a normal stale result"),
            RecordRepresentationContentIdentityStatus::StaleSource
        );
        assert_eq!(
            catalog
                .relink_match(&identity)
                .expect("old identity stays absent"),
            None
        );
    }

    #[test]
    fn likes_are_independent_from_stars_and_flags_and_survive_reopen() {
        let root = std::env::temp_dir().join(format!("shadow-library-like-{}", PhotoId::new_v7()));
        std::fs::create_dir_all(&root).expect("create root");
        let database = root.join("library.sqlite");
        let photo_id;
        {
            let mut catalog = Catalog::open(&database).expect("open catalog");
            photo_id = register(&mut catalog, "/archive/like.nef").photo_id;
            catalog
                .set_photo_library_state(&SetPhotoLibraryState {
                    photo_id,
                    liked: true,
                    color_label: "blue".into(),
                    updated_at_ms: 100,
                })
                .expect("like photo");
        }
        let catalog = Catalog::open(&database).expect("reopen catalog");
        let state = catalog.photo_library_state(photo_id).expect("read state");
        assert!(state.liked);
        assert_eq!(state.color_label, "blue");
        drop(catalog);
        std::fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn one_photo_can_belong_to_multiple_manual_albums() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo = register(&mut catalog, "/archive/albums.nef").photo_id;
        let first = catalog
            .create_library_album(AlbumKind::Manual, "Travel", None, 10)
            .expect("first album");
        let second = catalog
            .create_library_album(AlbumKind::Manual, "Favorites", None, 20)
            .expect("second album");
        catalog
            .add_photo_to_album(first.id, photo, 0, 30)
            .expect("add first membership");
        catalog
            .add_photo_to_album(second.id, photo, 0, 31)
            .expect("add second membership");
        // Repeated add is intentionally idempotent.
        catalog
            .add_photo_to_album(first.id, photo, 9, 32)
            .expect("idempotent membership");
        assert_eq!(catalog.albums_for_photo(photo).expect("albums").len(), 2);
        assert!(
            catalog
                .remove_photo_from_album(first.id, photo)
                .expect("remove membership")
        );
        let albums = catalog.albums_for_photo(photo).expect("remaining album");
        assert_eq!(albums, vec![second]);
    }

    #[test]
    fn smart_album_query_v1_is_canonical_validated_and_uses_the_indexed_grid() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let matching = register(&mut catalog, "/archive/smart-match.nef");
        let excluded = register(&mut catalog, "/archive/smart-excluded.nef");
        catalog
            .upsert_photo_library_facts(&facts_for(
                matching,
                Some(1_700_000_200),
                "Nikon Corporation",
                "Nikon Z 8",
            ))
            .expect("index matching facts");
        catalog
            .upsert_photo_library_facts(&facts_for(excluded, Some(1_700_000_100), "Pentax", "K10D"))
            .expect("index excluded facts");
        catalog
            .set_photo_library_state(&SetPhotoLibraryState {
                photo_id: matching.photo_id,
                liked: true,
                color_label: "none".into(),
                updated_at_ms: 30,
            })
            .expect("like matching photo");

        let query = SmartAlbumQueryV1::new(LibraryPhotoFilter {
            camera_key: Some(library_equipment_key("Nikon Corporation", "Nikon Z 8")),
            liked: Some(true),
            ..LibraryPhotoFilter::default()
        })
        .expect("build smart query");
        let query_json = query.to_json().expect("serialize smart query");
        assert_eq!(query.schema_version(), SmartAlbumQueryV1::SCHEMA_VERSION);
        assert_eq!(
            SmartAlbumQueryV1::from_json(&query_json)
                .expect("read canonical query")
                .library_filter()
                .expect("get executable filter"),
            query.library_filter().expect("get original filter")
        );

        let album = catalog
            .create_smart_library_album("Nikon favorites", &query, 40)
            .expect("create smart album");
        assert_eq!(album.query_json.as_deref(), Some(query_json.as_str()));
        let page = catalog
            .smart_album_photo_page(album.id, None, 16)
            .expect("page smart album");
        assert_eq!(page.items.len(), 1);
        assert_eq!(page.items[0].photo_id, matching.photo_id);
        assert_eq!(
            catalog
                .smart_album_photo_count(album.id)
                .expect("count smart album"),
            1
        );

        assert!(
            catalog
                .create_library_album(AlbumKind::Smart, "Invalid", Some("{}"), 41)
                .is_err()
        );
        assert!(SmartAlbumQueryV1::from_json(r#"{"schema_version":2,"filter":{}}"#).is_err());
        assert!(
            SmartAlbumQueryV1::from_json(r#"{"schema_version":1,"filter":{},"typo":true}"#)
                .is_err()
        );
        let membership_query = format!(
            r#"{{"schema_version":1,"filter":{{"album_id":"{}"}}}}"#,
            CollectionId::new_v7()
        );
        assert!(SmartAlbumQueryV1::from_json(&membership_query).is_err());
    }

    #[test]
    fn album_management_renames_replaces_queries_and_deletes_only_the_album() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo = register(&mut catalog, "/archive/album-management.nef");
        let manual = catalog
            .create_library_album(AlbumKind::Manual, "Before", None, 10)
            .expect("create manual album");
        catalog
            .add_photo_to_album(manual.id, photo.photo_id, 0, 11)
            .expect("add manual membership");
        let renamed = catalog
            .rename_library_album(manual.id, "After", 12)
            .expect("rename manual album");
        assert_eq!(renamed.name, "After");
        assert_eq!(renamed.updated_at_ms, 12);

        let initial_query = SmartAlbumQueryV1::new(LibraryPhotoFilter::default())
            .expect("create initial smart query");
        let smart = catalog
            .create_smart_library_album("All photos", &initial_query, 20)
            .expect("create smart album");
        let refined_query = SmartAlbumQueryV1::new(LibraryPhotoFilter {
            liked: Some(true),
            ..LibraryPhotoFilter::default()
        })
        .expect("create refined smart query");
        let updated = catalog
            .replace_smart_album_query(smart.id, &refined_query, 21)
            .expect("replace smart query");
        assert_eq!(
            updated.query_json,
            Some(refined_query.to_json().expect("serialize query"))
        );
        assert_eq!(
            catalog
                .smart_album_filter(smart.id)
                .expect("read replaced smart query"),
            refined_query
                .library_filter()
                .expect("read expected filter")
        );
        assert!(
            catalog
                .replace_smart_album_query(manual.id, &initial_query, 22)
                .is_err()
        );

        assert!(
            catalog
                .delete_library_album(manual.id)
                .expect("delete manual album")
        );
        assert!(
            catalog
                .albums_for_photo(photo.photo_id)
                .expect("read memberships after deletion")
                .is_empty()
        );
        assert_eq!(
            catalog
                .library_photo_count(&LibraryPhotoFilter::default())
                .expect("photo remains in Library"),
            1
        );
        assert!(
            !catalog
                .delete_library_album(manual.id)
                .expect("deleting absent album is idempotent")
        );
    }

    #[test]
    fn facts_are_indexed_with_their_source_provenance() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let registered = register(&mut catalog, "/archive/facts.nef");
        let facts = LibraryPhotoFacts {
            photo_id: registered.photo_id,
            captured_at_unix_seconds: Some(1_700_000_000),
            capture_day: "2023-11-14".into(),
            camera_make: "NIKON CORPORATION".into(),
            camera_model: "NIKON Z 8".into(),
            lens_make: "Nikon".into(),
            lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
            aperture_milli: Some(4_000),
            focal_length_tenth_mm: Some(240),
            iso_speed: Some(800.0),
            latitude_e7: Some(399_000_000),
            longitude_e7: Some(116_400_0000),
            place_name: "Beijing".into(),
            indexed_representation_id: Some(registered.representation_id),
            indexed_source: Some(RepresentationFingerprint {
                byte_len: 100,
                modified_at_ms: Some(10),
            }),
            indexed_at_ms: 100,
        };
        catalog
            .upsert_photo_library_facts(&facts)
            .expect("record facts");
        assert_eq!(
            catalog
                .photo_library_facts(registered.photo_id)
                .expect("read facts"),
            Some(facts)
        );
    }

    fn facts_for(
        registered: RegisteredAsset,
        captured_at_unix_seconds: Option<i64>,
        make: &str,
        model: &str,
    ) -> LibraryPhotoFacts {
        LibraryPhotoFacts {
            photo_id: registered.photo_id,
            captured_at_unix_seconds,
            capture_day: captured_at_unix_seconds
                .map(|_| "2023-11-14".to_owned())
                .unwrap_or_default(),
            camera_make: make.into(),
            camera_model: model.into(),
            lens_make: "Nikon".into(),
            lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
            aperture_milli: Some(4_000),
            focal_length_tenth_mm: Some(240),
            iso_speed: Some(800.0),
            latitude_e7: None,
            longitude_e7: None,
            place_name: String::new(),
            indexed_representation_id: Some(registered.representation_id),
            indexed_source: Some(RepresentationFingerprint {
                byte_len: 100,
                modified_at_ms: Some(10),
            }),
            indexed_at_ms: 100,
        }
    }

    #[test]
    fn photo_first_library_page_filters_facets_and_keysets_without_path_ownership() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let newest = register(&mut catalog, "/one/source/first.nef");
        let middle = register(&mut catalog, "/other/source/second.nef");
        let unindexed = register(&mut catalog, "/third/source/third.nef");
        catalog
            .upsert_photo_library_facts(&facts_for(
                newest,
                Some(1_700_000_200),
                "Nikon Corporation",
                "Nikon Z 8",
            ))
            .expect("newest facts");
        catalog
            .upsert_photo_library_facts(&facts_for(middle, Some(1_700_000_100), "Pentax", "K10D"))
            .expect("middle facts");
        catalog
            .set_photo_library_state(&SetPhotoLibraryState {
                photo_id: newest.photo_id,
                liked: true,
                color_label: "blue".into(),
                updated_at_ms: 200,
            })
            .expect("like newest");
        catalog
            .append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: "library-filter-picked".into(),
                photo_id: newest.photo_id,
                occurred_at_unix_ms: 201,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence: 0,
                before_flag: PhotoFlag::Unflagged,
                before_rating: 0,
                after_flag: PhotoFlag::Picked,
                after_rating: 5,
            })
            .expect("pick newest");
        let album = catalog
            .create_library_album(AlbumKind::Manual, "Portfolio", None, 202)
            .expect("album");
        catalog
            .add_photo_to_album(album.id, newest.photo_id, 0, 203)
            .expect("membership");
        let working_recipe = RecipeCommit::new(
            RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            Vec::new(),
            RecipeSnapshot::empty(),
            Some("Library filter fixture".into()),
            204,
        )
        .expect("create working Recipe");
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id: middle.photo_id,
                commit: working_recipe,
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: None,
                }],
            })
            .expect("persist working Recipe");

        let exact = catalog
            .library_photo_page(
                &LibraryPhotoFilter {
                    camera_key: Some(library_equipment_key("Nikon Corporation", "Nikon Z 8")),
                    lens_key: Some(library_equipment_key("Nikon", "NIKKOR Z 24-120mm f/4 S")),
                    aperture: Some(LibraryApertureRange {
                        minimum_milli: Some(4_000),
                        maximum_milli: Some(4_000),
                    }),
                    liked: Some(true),
                    color_label: Some("blue".into()),
                    flag: Some(PhotoFlag::Picked),
                    minimum_rating: Some(3),
                    album_id: Some(album.id),
                    ..LibraryPhotoFilter::default()
                },
                None,
                16,
            )
            .expect("all facets");
        assert_eq!(
            catalog
                .library_photo_count(&LibraryPhotoFilter {
                    camera_key: Some(library_equipment_key("Nikon Corporation", "Nikon Z 8")),
                    lens_key: Some(library_equipment_key("Nikon", "NIKKOR Z 24-120mm f/4 S")),
                    aperture: Some(LibraryApertureRange {
                        minimum_milli: Some(4_000),
                        maximum_milli: Some(4_000),
                    }),
                    liked: Some(true),
                    color_label: Some("blue".into()),
                    flag: Some(PhotoFlag::Picked),
                    minimum_rating: Some(3),
                    album_id: Some(album.id),
                    ..LibraryPhotoFilter::default()
                })
                .expect("exact count after facets settle"),
            1
        );
        assert_eq!(exact.items.len(), 1);
        assert_eq!(exact.items[0].photo_id, newest.photo_id);
        assert_eq!(
            exact.items[0].location.display_path,
            "/one/source/first.nef"
        );
        assert!(exact.items[0].state.liked);
        assert_eq!(exact.items[0].decision.flag, PhotoFlag::Picked);

        let edited = catalog
            .library_photo_page(
                &LibraryPhotoFilter {
                    has_development_edits: Some(true),
                    ..LibraryPhotoFilter::default()
                },
                None,
                16,
            )
            .expect("read edited Library page");
        assert_eq!(edited.items.len(), 1);
        assert_eq!(edited.items[0].photo_id, middle.photo_id);
        assert!(edited.items[0].has_development_edits);
        assert_eq!(
            catalog
                .library_photo_count(&LibraryPhotoFilter {
                    has_development_edits: Some(true),
                    ..LibraryPhotoFilter::default()
                })
                .expect("count edited Library photos"),
            1
        );

        let unedited = catalog
            .library_photo_page(
                &LibraryPhotoFilter {
                    has_development_edits: Some(false),
                    ..LibraryPhotoFilter::default()
                },
                None,
                16,
            )
            .expect("read unedited Library page");
        assert_eq!(unedited.items.len(), 2);
        assert!(
            unedited
                .items
                .iter()
                .all(|item| !item.has_development_edits)
        );

        let first_page = catalog
            .library_photo_page(&LibraryPhotoFilter::default(), None, 2)
            .expect("first page");
        assert_eq!(
            catalog
                .library_photo_count(&LibraryPhotoFilter::default())
                .expect("count all photos"),
            3
        );
        assert_eq!(
            first_page
                .items
                .iter()
                .map(|item| item.photo_id)
                .collect::<Vec<_>>(),
            vec![newest.photo_id, middle.photo_id]
        );
        let second_page = catalog
            .library_photo_page(
                &LibraryPhotoFilter::default(),
                first_page.next_cursor.as_ref(),
                2,
            )
            .expect("second page");
        assert_eq!(
            second_page
                .items
                .iter()
                .map(|item| item.photo_id)
                .collect::<Vec<_>>(),
            vec![unindexed.photo_id]
        );
        assert!(second_page.next_cursor.is_none());
    }

    #[test]
    fn bounded_library_facets_compose_without_directory_ownership() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let nikon_one = register(&mut catalog, "/roots/a/nikon-one.nef");
        let nikon_two = register(&mut catalog, "/roots/b/nikon-two.nef");
        let nikon_three = register(&mut catalog, "/roots/c/nikon-three.nef");
        let canon = register(&mut catalog, "/roots/d/canon.cr3");

        let mut facts = facts_for(
            nikon_one,
            Some(1_700_000_000),
            "Nikon Corporation",
            "Nikon Z 8",
        );
        facts.capture_day = "2024-03-18".into();
        catalog.upsert_photo_library_facts(&facts).expect("first nikon facts");

        let mut facts = facts_for(
            nikon_two,
            Some(1_700_000_100),
            "Nikon Corporation",
            "Nikon Z 8",
        );
        facts.capture_day = "2024-03-03".into();
        catalog.upsert_photo_library_facts(&facts).expect("second nikon facts");

        let mut facts = facts_for(
            nikon_three,
            Some(1_700_000_200),
            "Nikon Corporation",
            "Nikon Z 8",
        );
        facts.capture_day = "2024-02-16".into();
        facts.lens_model = "NIKKOR Z 50mm f/1.8 S".into();
        catalog.upsert_photo_library_facts(&facts).expect("third nikon facts");

        let mut facts = facts_for(canon, Some(1_700_000_300), "Canon", "EOS R5");
        facts.capture_day = "2024-01-07".into();
        facts.lens_make = "Canon".into();
        facts.lens_model = "RF 24-105mm F4 L IS USM".into();
        catalog.upsert_photo_library_facts(&facts).expect("canon facts");

        let cameras = catalog
            .library_facet_page(&LibraryPhotoFilter::default(), LibraryFacetKind::Camera, None, 1)
            .expect("first camera facet page");
        assert_eq!(cameras.items.len(), 1);
        assert_eq!(cameras.items[0].label, "Nikon Corporation Nikon Z 8");
        assert_eq!(cameras.items[0].photo_count, 3);
        let remaining_cameras = catalog
            .library_facet_page(
                &LibraryPhotoFilter::default(),
                LibraryFacetKind::Camera,
                cameras.next_cursor.as_ref(),
                1,
            )
            .expect("second camera facet page");
        assert_eq!(remaining_cameras.items.len(), 1);
        assert_eq!(remaining_cameras.items[0].label, "Canon EOS R5");

        let months = catalog
            .library_facet_page(
                &LibraryPhotoFilter::default(),
                LibraryFacetKind::CaptureMonth,
                None,
                16,
            )
            .expect("month facets");
        assert_eq!(months.items[0], LibraryFacetValue {
            key: "2024-03".into(),
            label: "2024-03".into(),
            photo_count: 2,
        });
        assert_eq!(months.items.len(), 3);

        let march = LibraryPhotoFilter {
            capture_month: Some("2024-03".into()),
            ..LibraryPhotoFilter::default()
        };
        assert_eq!(catalog.library_photo_count(&march).expect("march count"), 2);
        let lenses = catalog
            .library_facet_page(&march, LibraryFacetKind::Lens, None, 16)
            .expect("month-constrained lens facets");
        assert_eq!(lenses.items.len(), 1);
        assert_eq!(lenses.items[0].photo_count, 2);
        assert_eq!(lenses.items[0].label, "Nikon NIKKOR Z 24-120mm f/4 S");

        assert!(catalog
            .library_photo_count(&LibraryPhotoFilter {
                capture_month: Some("2024-13".into()),
                ..LibraryPhotoFilter::default()
            })
            .is_err());
    }

    #[test]
    fn source_health_pages_scan_absences_without_marking_locations_offline() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let root = AssetLocation::new(Platform::MacOs, b"/archive".to_vec(), "/archive");
        let first_scan = catalog
            .begin_import_session(&root, 1)
            .expect("begin first source scan");
        let first = register_scan_entry(&mut catalog, first_scan, "/archive/first.nef", 2);
        let second = register_scan_entry(&mut catalog, first_scan, "/archive/second.nef", 3);
        let third = register_scan_entry(&mut catalog, first_scan, "/archive/third.nef", 4);
        catalog
            .finish_import_session(first_scan, ImportSessionState::Completed, None, 5)
            .expect("finish first source scan");

        let second_scan = catalog
            .begin_import_session(&root, 10)
            .expect("begin second source scan");
        let observed = register_scan_entry(&mut catalog, second_scan, "/archive/first.nef", 11);
        assert_eq!(observed.representation_id, first.representation_id);
        catalog
            .finish_import_session(second_scan, ImportSessionState::Completed, None, 12)
            .expect("finish second source scan");

        let health = catalog.library_source_health().expect("read source health");
        assert_eq!(health.len(), 1);
        let scan = health[0]
            .latest_completed_scan
            .as_ref()
            .expect("completed scan evidence");
        assert_eq!(scan.session_id, second_scan);
        assert_eq!(scan.known_locations, 3);
        assert_eq!(scan.seen_locations, 1);
        assert_eq!(scan.not_seen_locations, 2);

        let first_page = catalog
            .missing_source_location_page(second_scan, None, 1)
            .expect("page missing locations")
            .expect("durable source page");
        assert_eq!(first_page.reconciliation, *scan);
        assert_eq!(first_page.items.len(), 1);
        let relink_target = catalog
            .missing_source_relink_target(second_scan, first_page.items[0].location_id)
            .expect("read exact source relink target")
            .expect("first-page item remains valid reattach evidence");
        assert_eq!(relink_target.location, first_page.items[0]);
        assert!(catalog
            .missing_source_relink_target(second_scan, observed.location_id)
            .expect("check observed source")
            .is_none());

        // Refresh the *other* missing location in a later scan. The review
        // must still page the old completed session without skipping it just
        // because mutable `last_seen_at_ms` changed in the meantime.
        let refreshed_representation =
            if first_page.items[0].representation_id == second.representation_id {
                third.representation_id
            } else {
                second.representation_id
            };
        let refreshed_path = if refreshed_representation == second.representation_id {
            "/archive/second.nef"
        } else {
            "/archive/third.nef"
        };
        let later_scan = catalog
            .begin_import_session(&root, 20)
            .expect("begin later source scan");
        let refreshed = register_scan_entry(&mut catalog, later_scan, refreshed_path, 21);
        assert_eq!(refreshed.representation_id, refreshed_representation);
        catalog
            .finish_import_session(later_scan, ImportSessionState::Completed, None, 22)
            .expect("finish later source scan");

        let second_page = catalog
            .missing_source_location_page(second_scan, first_page.next_cursor.as_ref(), 1)
            .expect("page remaining missing locations")
            .expect("durable source page");
        assert_eq!(second_page.items.len(), 1);
        assert!(second_page.next_cursor.is_none());

        let missing = first_page
            .items
            .iter()
            .chain(second_page.items.iter())
            .map(|item| item.representation_id)
            .collect::<Vec<_>>();
        assert_eq!(missing.len(), 2);
        assert!(missing.contains(&second.representation_id));
        assert!(missing.contains(&third.representation_id));
        assert_ne!(missing[0], missing[1]);
        assert!(
            first_page
                .items
                .iter()
                .chain(second_page.items.iter())
                .all(|item| item.location.display_path.starts_with("/archive/"))
        );

        // A source-specific absence does not make the source unavailable to
        // the normal photo-first Library query.
        assert_eq!(
            catalog
                .library_photo_count(&LibraryPhotoFilter::default())
                .expect("count online Library originals"),
            3
        );
    }
}

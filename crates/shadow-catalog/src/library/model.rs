//! Public value objects and validated query contracts for the photo-first Library.
//!
//! Start here to understand the durable Library vocabulary. SQLite query and mutation mechanics
//! remain in the parent module.

use serde::{Deserialize, Serialize};
use shadow_domain::{
    AssetLocation, CollectionId, LibrarySourceId, LocationId, PhotoDecisionState, PhotoFlag,
    PhotoId, RepresentationId, RepresentationKind,
};

use crate::{CatalogError, RepresentationFingerprint, SourceScanReconciliation};

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
    pub(super) const fn as_str(self) -> &'static str {
        match self {
            Self::Manual => "manual",
            Self::Smart => "smart",
        }
    }

    pub(super) fn parse(value: &str) -> Result<Self, CatalogError> {
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

pub(super) fn validate_library_photo_filter(
    filter: &LibraryPhotoFilter,
) -> Result<(), CatalogError> {
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

pub(super) fn capture_month_bounds(capture_month: &str) -> Option<(String, String)> {
    let bytes = capture_month.as_bytes();
    if bytes.len() != 7
        || bytes[4] != b'-'
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

pub(super) fn normalize_query_key(value: &str) -> String {
    value.trim().to_ascii_lowercase()
}

pub(super) fn validate_facts(facts: &LibraryPhotoFacts) -> Result<(), CatalogError> {
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

pub(super) fn validate_library_state(state: &SetPhotoLibraryState) -> Result<(), CatalogError> {
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

pub(super) fn validate_album(
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

pub(super) fn normalized_equipment_key(make: &str, model: &str) -> String {
    [make.trim(), model.trim()]
        .into_iter()
        .filter(|value| !value.is_empty())
        .map(|value| value.to_ascii_lowercase())
        .collect::<Vec<_>>()
        .join("\u{001f}")
}

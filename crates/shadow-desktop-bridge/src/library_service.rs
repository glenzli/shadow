//! Photo-first Library queries for the desktop bridge.
//!
//! This service deliberately owns only the conversion between the stable,
//! path-independent Catalog Library projection and the compact CXX DTO used by
//! the desktop shell.  Review remains a separate, session-local visual and
//! comparison service: replacing its grid query must not change the evidence
//! or signed-preview contract.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    AlbumKind, AlbumRecord, CatalogHandle, LibraryApertureRange, LibraryDateRange,
    LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryPhotoCursor,
    LibraryPhotoFilter, LibraryPhotoPage, LibraryPhotoRecord,
    LibrarySourceHealth, MissingSourceLocationCursor, MissingSourceLocationPage,
    SetPhotoLibraryState, SmartAlbumQueryV1,
};
use shadow_domain::{CollectionId, ImportSessionId, LocationId, PhotoFlag, PhotoId};

use crate::{
    ffi,
    review_service::{GridVisualPresentation, ReviewService, file_name},
};

/// Stateless facade for the catalog's photo-first Library projection.
///
/// Keeping this distinct from [`crate::review_service::ReviewService`] makes
/// the ownership boundary explicit: Library pages are durable catalog queries,
/// while Review only supplies the existing signed-handle and transient-preview
/// presentation contract for the selected page artifacts.
#[derive(Debug, Clone)]
pub(crate) struct LibraryService {
    catalog: CatalogHandle,
}

impl LibraryService {
    pub(crate) fn new(catalog: CatalogHandle) -> Self {
        Self { catalog }
    }

    /// Reads one bounded, keyset-paginated photo-first page.
    pub(crate) fn photo_page(
        &self,
        review: &ReviewService,
        ffi_filter: &ffi::FfiLibraryPhotoFilter,
        ffi_cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        let filter = library_filter_from_ffi(ffi_filter)?;
        let cursor = library_cursor_from_ffi(ffi_cursor)?;
        let page = self.catalog.library_photo_page(
            &filter,
            cursor.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
        )?;
        self.present_photo_page(review, page)
    }

    /// Counts a settled filter. The desktop grid should not call this during
    /// scroll; it is intentionally separate from [`Self::photo_page`].
    pub(crate) fn photo_count(&self, ffi_filter: &ffi::FfiLibraryPhotoFilter) -> AnyResult<u64> {
        let filter = library_filter_from_ffi(ffi_filter)?;
        Ok(self.catalog.library_photo_count(&filter)?)
    }

    /// Reads a bounded aggregation page for one metadata facet. The native
    /// Catalog determines both keys and labels; Qt only receives a compact
    /// typed list and cannot substitute a folder hierarchy for it.
    pub(crate) fn facet_page(
        &self,
        ffi_filter: &ffi::FfiLibraryPhotoFilter,
        kind: ffi::FfiLibraryFacetKind,
        cursor: &ffi::FfiLibraryFacetCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryFacetPage> {
        let filter = library_filter_from_ffi(ffi_filter)?;
        let kind = library_facet_kind_from_ffi(kind)?;
        let cursor = library_facet_cursor_from_ffi(cursor)?;
        let page = self.catalog.library_facet_page(
            &filter,
            kind,
            cursor.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
        )?;
        Ok(ffi_library_facet_page(page))
    }

    /// Presents every durable album to the CXX facade without exposing raw
    /// stored JSON. Smart queries round-trip through their typed v1 filter.
    pub(crate) fn ffi_albums(&self) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
        ffi_library_albums(self.albums()?)
    }

    /// Projects only observational source-scan evidence. The desktop must
    /// never infer a global missing file or a replacement path from this list.
    pub(crate) fn ffi_source_health(&self) -> AnyResult<Vec<ffi::FfiLibrarySourceHealth>> {
        self.catalog
            .library_source_health()?
            .into_iter()
            .map(ffi_library_source_health)
            .collect()
    }

    /// Reads a bounded page of locations absent from one completed source
    /// scan. This remains strictly observational: reattach planning and any
    /// later confirmation live behind a separate, explicit workflow.
    pub(crate) fn ffi_missing_source_location_page(
        &self,
        scan_session_id: &str,
        after_location_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiMissingSourceLocationPage> {
        let scan_session_id = scan_session_id
            .trim()
            .parse::<ImportSessionId>()
            .with_context(|| format!("parse source-health scan session id {scan_session_id}"))?;
        let after = if after_location_id.trim().is_empty() {
            None
        } else {
            Some(MissingSourceLocationCursor {
                location_id: after_location_id
                    .trim()
                    .parse::<LocationId>()
                    .with_context(|| {
                        format!("parse source-health location cursor {after_location_id}")
                    })?,
            })
        };
        let page = self.catalog.missing_source_location_page(
            scan_session_id,
            after.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
        )?;
        Ok(page.map_or_else(empty_ffi_missing_source_location_page, ffi_missing_source_location_page))
    }

    /// Lists the durable user albums. The desktop uses this for its Library
    /// sidebar; it never infers albums from import directories.
    pub(crate) fn albums(&self) -> AnyResult<Vec<AlbumRecord>> {
        Ok(self.catalog.library_albums()?)
    }

    /// Creates one membership-backed manual album.
    pub(crate) fn create_manual_album(&self, name: &str, now_ms: i64) -> AnyResult<AlbumRecord> {
        Ok(self
            .catalog
            .create_library_album(AlbumKind::Manual, name, None, now_ms)?)
    }

    pub(crate) fn create_manual_album_ffi(
        &self,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        ffi_library_album(self.create_manual_album(name, now_ms)?)
    }

    /// Creates one query-backed smart album from the typed v1 contract.
    pub(crate) fn create_smart_album(
        &self,
        name: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> AnyResult<AlbumRecord> {
        Ok(self
            .catalog
            .create_smart_library_album(name, query, now_ms)?)
    }

    pub(crate) fn create_smart_album_ffi(
        &self,
        name: &str,
        query_filter: &ffi::FfiLibraryPhotoFilter,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        let query = smart_album_query_from_ffi(query_filter)?;
        ffi_library_album(self.create_smart_album(name, &query, now_ms)?)
    }

    pub(crate) fn rename_album(
        &self,
        album_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<AlbumRecord> {
        Ok(self.catalog.rename_library_album(
            library_album_id_from_text(album_id)?,
            name,
            now_ms,
        )?)
    }

    pub(crate) fn rename_album_ffi(
        &self,
        album_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        ffi_library_album(self.rename_album(album_id, name, now_ms)?)
    }

    pub(crate) fn replace_smart_album_query(
        &self,
        album_id: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> AnyResult<AlbumRecord> {
        Ok(self.catalog.replace_smart_album_query(
            library_album_id_from_text(album_id)?,
            query,
            now_ms,
        )?)
    }

    pub(crate) fn replace_smart_album_query_ffi(
        &self,
        album_id: &str,
        query_filter: &ffi::FfiLibraryPhotoFilter,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        let query = smart_album_query_from_ffi(query_filter)?;
        ffi_library_album(self.replace_smart_album_query(album_id, &query, now_ms)?)
    }

    /// Deletes an album without deleting any photo it previously contained.
    pub(crate) fn delete_album(&self, album_id: &str) -> AnyResult<bool> {
        Ok(self
            .catalog
            .delete_library_album(library_album_id_from_text(album_id)?)?)
    }

    /// Adds a photo to one manual album. The Catalog rejects a smart album so
    /// callers cannot accidentally turn a computed album into a static one.
    pub(crate) fn add_photo_to_manual_album(
        &self,
        album_id: &str,
        photo_id: &str,
        sort_key: i64,
        now_ms: i64,
    ) -> AnyResult<()> {
        self.catalog.add_photo_to_album(
            library_album_id_from_text(album_id)?,
            library_photo_id_from_text(photo_id)?,
            sort_key,
            now_ms,
        )?;
        Ok(())
    }

    pub(crate) fn remove_photo_from_manual_album(
        &self,
        album_id: &str,
        photo_id: &str,
    ) -> AnyResult<bool> {
        Ok(self.catalog.remove_photo_from_album(
            library_album_id_from_text(album_id)?,
            library_photo_id_from_text(photo_id)?,
        )?)
    }

    pub(crate) fn albums_for_photo(&self, photo_id: &str) -> AnyResult<Vec<AlbumRecord>> {
        Ok(self
            .catalog
            .albums_for_photo(library_photo_id_from_text(photo_id)?)?)
    }

    pub(crate) fn ffi_albums_for_photo(
        &self,
        photo_id: &str,
    ) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
        ffi_library_albums(self.albums_for_photo(photo_id)?)
    }

    /// Renders a smart album through the same cached-artifact presentation
    /// path as an ordinary Library page.
    pub(crate) fn smart_album_photo_page(
        &self,
        review: &ReviewService,
        album_id: &str,
        ffi_cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        let album_id = library_album_id_from_text(album_id)?;
        let cursor = library_cursor_from_ffi(ffi_cursor)?;
        let page = self.catalog.smart_album_photo_page(
            album_id,
            cursor.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
        )?;
        self.present_photo_page(review, page)
    }

    pub(crate) fn smart_album_photo_count(&self, album_id: &str) -> AnyResult<u64> {
        Ok(self
            .catalog
            .smart_album_photo_count(library_album_id_from_text(album_id)?)?)
    }

    /// Replaces only the mutable Library-affinity projection for one logical
    /// photo. Review flags and ratings deliberately use the separate,
    /// append-only decision service.
    pub(crate) fn set_photo_library_state(
        &self,
        photo_id: &str,
        liked: bool,
        color_label: &str,
        updated_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoLibraryState> {
        let photo_id = photo_id
            .parse::<PhotoId>()
            .with_context(|| format!("parse Library photo id {photo_id}"))?;
        self.catalog
            .set_photo_library_state(&SetPhotoLibraryState {
                photo_id,
                liked,
                color_label: color_label.to_owned(),
                updated_at_ms,
            })?;
        let state = self.catalog.photo_library_state(photo_id)?;
        Ok(ffi::FfiPhotoLibraryState {
            photo_id: state.photo_id.to_string(),
            liked: state.liked,
            color_label: state.color_label,
            updated_at_ms: state.updated_at_ms,
        })
    }

    fn present_photo_page(
        &self,
        review: &ReviewService,
        page: LibraryPhotoPage,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        let representation_ids = page
            .items
            .iter()
            .map(|record| record.representation_id)
            .collect::<Vec<_>>();
        let visuals = self
            .catalog
            .preferred_cached_artifacts(&representation_ids)?;
        ffi_library_page(page, visuals, review)
    }
}

fn library_filter_from_ffi(filter: &ffi::FfiLibraryPhotoFilter) -> AnyResult<LibraryPhotoFilter> {
    let capture_time =
        (filter.has_capture_start || filter.has_capture_end).then_some(LibraryDateRange {
            start_inclusive: filter
                .has_capture_start
                .then_some(filter.capture_start_unix_seconds),
            end_inclusive: filter
                .has_capture_end
                .then_some(filter.capture_end_unix_seconds),
        });
    let aperture = (filter.has_aperture_minimum || filter.has_aperture_maximum).then_some(
        LibraryApertureRange {
            minimum_milli: filter
                .has_aperture_minimum
                .then_some(filter.aperture_minimum_milli),
            maximum_milli: filter
                .has_aperture_maximum
                .then_some(filter.aperture_maximum_milli),
        },
    );
    let flag = match filter.flag {
        ffi::FfiLibraryFlagFilter::Any => None,
        ffi::FfiLibraryFlagFilter::Unflagged => Some(PhotoFlag::Unflagged),
        ffi::FfiLibraryFlagFilter::Picked => Some(PhotoFlag::Picked),
        ffi::FfiLibraryFlagFilter::Rejected => Some(PhotoFlag::Rejected),
        _ => bail!("unknown Library flag filter"),
    };
    let album_id = optional_filter_text(&filter.album_id)
        .as_deref()
        .map(library_album_id_from_text)
        .transpose()?;

    Ok(LibraryPhotoFilter {
        capture_time,
        capture_month: optional_filter_text(&filter.capture_month),
        camera_key: optional_filter_text(&filter.camera_key),
        lens_key: optional_filter_text(&filter.lens_key),
        aperture,
        liked: filter.has_liked.then_some(filter.liked),
        color_label: optional_filter_text(&filter.color_label),
        flag,
        minimum_rating: filter.has_minimum_rating.then_some(filter.minimum_rating),
        has_development_edits: filter
            .has_development_edits
            .then_some(filter.development_edits),
        album_id,
    })
}

fn optional_filter_text(value: &str) -> Option<String> {
    let trimmed = value.trim();
    (!trimmed.is_empty()).then(|| trimmed.to_owned())
}

fn smart_album_query_from_ffi(filter: &ffi::FfiLibraryPhotoFilter) -> AnyResult<SmartAlbumQueryV1> {
    Ok(SmartAlbumQueryV1::new(library_filter_from_ffi(filter)?)?)
}

fn ffi_library_albums(albums: Vec<AlbumRecord>) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
    albums.into_iter().map(ffi_library_album).collect()
}

fn ffi_library_source_health(
    health: LibrarySourceHealth,
) -> AnyResult<ffi::FfiLibrarySourceHealth> {
    let source_id = health.source.id.to_string();
    let source_display_path = health.source.root.display_path;
    let source_enabled = health.source.enabled;
    let (
        has_latest_completed_scan,
        scan_session_id,
        scan_completed_at_ms,
        known_locations,
        seen_locations,
        not_seen_locations,
    ) = match health.latest_completed_scan {
        Some(scan) => (
            true,
            scan.session_id.to_string(),
            scan.completed_at_ms,
            scan.known_locations,
            scan.seen_locations,
            scan.not_seen_locations,
        ),
        None => (false, String::new(), 0, 0, 0, 0),
    };
    Ok(ffi::FfiLibrarySourceHealth {
        source_id,
        source_display_path,
        source_enabled,
        has_latest_completed_scan,
        scan_session_id,
        scan_completed_at_ms,
        known_locations,
        seen_locations,
        not_seen_locations,
    })
}

fn empty_ffi_missing_source_location_page() -> ffi::FfiMissingSourceLocationPage {
    ffi::FfiMissingSourceLocationPage {
        has_scan: false,
        items: Vec::new(),
        has_more: false,
        next_location_id: String::new(),
    }
}

fn ffi_missing_source_location_page(
    page: MissingSourceLocationPage,
) -> ffi::FfiMissingSourceLocationPage {
    let next_location_id = page
        .next_cursor
        .map_or_else(String::new, |cursor| cursor.location_id.to_string());
    ffi::FfiMissingSourceLocationPage {
        has_scan: true,
        items: page
            .items
            .into_iter()
            .map(|item| ffi::FfiMissingSourceLocation {
                location_id: item.location_id.to_string(),
                photo_id: item.photo_id.to_string(),
                title: file_name(&item.location.display_path),
                source_display_path: item.location.display_path,
                has_captured_at: item.captured_at_unix_seconds.is_some(),
                captured_at_unix_seconds: item.captured_at_unix_seconds.unwrap_or_default(),
                camera_key: item.camera_key,
                last_seen_at_ms: item.last_seen_at_ms,
            })
            .collect(),
        has_more: !next_location_id.is_empty(),
        next_location_id,
    }
}

fn ffi_library_album(album: AlbumRecord) -> AnyResult<ffi::FfiLibraryAlbum> {
    let (kind, query_filter) = match (album.kind, album.query_json.as_deref()) {
        (AlbumKind::Manual, None) => (ffi::FfiLibraryAlbumKind::Manual, empty_ffi_library_filter()),
        (AlbumKind::Manual, Some(_)) => bail!("manual album {} unexpectedly has a query", album.id),
        (AlbumKind::Smart, Some(query_json)) => {
            let query = SmartAlbumQueryV1::from_json(query_json)?;
            (
                ffi::FfiLibraryAlbumKind::Smart,
                ffi_library_filter(query.library_filter()?),
            )
        }
        (AlbumKind::Smart, None) => bail!("smart album {} is missing its query", album.id),
    };
    Ok(ffi::FfiLibraryAlbum {
        id: album.id.to_string(),
        kind,
        name: album.name,
        query_filter,
        created_at_ms: album.created_at_ms,
        updated_at_ms: album.updated_at_ms,
    })
}

fn empty_ffi_library_filter() -> ffi::FfiLibraryPhotoFilter {
    ffi_library_filter(LibraryPhotoFilter::default())
}

fn ffi_library_filter(filter: LibraryPhotoFilter) -> ffi::FfiLibraryPhotoFilter {
    let (has_capture_start, capture_start_unix_seconds, has_capture_end, capture_end_unix_seconds) =
        filter.capture_time.map_or((false, 0, false, 0), |range| {
            (
                range.start_inclusive.is_some(),
                range.start_inclusive.unwrap_or_default(),
                range.end_inclusive.is_some(),
                range.end_inclusive.unwrap_or_default(),
            )
        });
    let (
        has_aperture_minimum,
        aperture_minimum_milli,
        has_aperture_maximum,
        aperture_maximum_milli,
    ) = filter.aperture.map_or((false, 0, false, 0), |range| {
        (
            range.minimum_milli.is_some(),
            range.minimum_milli.unwrap_or_default(),
            range.maximum_milli.is_some(),
            range.maximum_milli.unwrap_or_default(),
        )
    });
    let flag = match filter.flag {
        None => ffi::FfiLibraryFlagFilter::Any,
        Some(PhotoFlag::Unflagged) => ffi::FfiLibraryFlagFilter::Unflagged,
        Some(PhotoFlag::Picked) => ffi::FfiLibraryFlagFilter::Picked,
        Some(PhotoFlag::Rejected) => ffi::FfiLibraryFlagFilter::Rejected,
    };
    ffi::FfiLibraryPhotoFilter {
        has_capture_start,
        capture_start_unix_seconds,
        has_capture_end,
        capture_end_unix_seconds,
        capture_month: filter.capture_month.unwrap_or_default(),
        camera_key: filter.camera_key.unwrap_or_default(),
        lens_key: filter.lens_key.unwrap_or_default(),
        has_aperture_minimum,
        aperture_minimum_milli,
        has_aperture_maximum,
        aperture_maximum_milli,
        has_liked: filter.liked.is_some(),
        liked: filter.liked.unwrap_or_default(),
        color_label: filter.color_label.unwrap_or_default(),
        flag,
        has_minimum_rating: filter.minimum_rating.is_some(),
        minimum_rating: filter.minimum_rating.unwrap_or_default(),
        has_development_edits: filter.has_development_edits.is_some(),
        development_edits: filter.has_development_edits.unwrap_or_default(),
        album_id: filter.album_id.map(|id| id.to_string()).unwrap_or_default(),
    }
}

fn library_facet_kind_from_ffi(
    kind: ffi::FfiLibraryFacetKind,
) -> AnyResult<LibraryFacetKind> {
    match kind {
        ffi::FfiLibraryFacetKind::CaptureMonth => Ok(LibraryFacetKind::CaptureMonth),
        ffi::FfiLibraryFacetKind::Camera => Ok(LibraryFacetKind::Camera),
        ffi::FfiLibraryFacetKind::Lens => Ok(LibraryFacetKind::Lens),
        _ => bail!("unknown Library facet kind"),
    }
}

fn library_facet_cursor_from_ffi(
    cursor: &ffi::FfiLibraryFacetCursor,
) -> AnyResult<Option<LibraryFacetCursor>> {
    if cursor.key.trim().is_empty() {
        if cursor.photo_count != 0 {
            bail!("Library facet cursor count requires a facet key");
        }
        return Ok(None);
    }
    Ok(Some(LibraryFacetCursor {
        photo_count: cursor.photo_count,
        key: cursor.key.trim().to_owned(),
    }))
}

fn ffi_library_facet_page(page: LibraryFacetPage) -> ffi::FfiLibraryFacetPage {
    let next_cursor = page.next_cursor.map_or_else(
        || ffi::FfiLibraryFacetCursor {
            photo_count: 0,
            key: String::new(),
        },
        |cursor| ffi::FfiLibraryFacetCursor {
            photo_count: cursor.photo_count,
            key: cursor.key,
        },
    );
    ffi::FfiLibraryFacetPage {
        items: page
            .items
            .into_iter()
            .map(|item| ffi::FfiLibraryFacet {
                key: item.key,
                label: item.label,
                photo_count: item.photo_count,
            })
            .collect(),
        has_more: !next_cursor.key.is_empty(),
        next_cursor,
    }
}

fn library_album_id_from_text(album_id: &str) -> AnyResult<CollectionId> {
    let album_id = album_id.trim();
    if album_id.is_empty() {
        bail!("Library album id is required");
    }
    album_id
        .parse::<CollectionId>()
        .with_context(|| format!("parse Library album id {album_id}"))
}

fn library_photo_id_from_text(photo_id: &str) -> AnyResult<PhotoId> {
    let photo_id = photo_id.trim();
    if photo_id.is_empty() {
        bail!("Library photo id is required");
    }
    photo_id
        .parse::<PhotoId>()
        .with_context(|| format!("parse Library photo id {photo_id}"))
}

fn library_cursor_from_ffi(
    cursor: &ffi::FfiLibraryPhotoCursor,
) -> AnyResult<Option<LibraryPhotoCursor>> {
    if cursor.photo_id.is_empty() {
        if cursor.has_capture_time {
            bail!("Library cursor capture time requires a photo id");
        }
        return Ok(None);
    }
    let photo_id = cursor
        .photo_id
        .parse::<PhotoId>()
        .with_context(|| format!("parse Library cursor photo id {}", cursor.photo_id))?;
    Ok(Some(LibraryPhotoCursor {
        captured_at_unix_seconds: cursor
            .has_capture_time
            .then_some(cursor.captured_at_unix_seconds),
        photo_id,
    }))
}

fn ffi_library_page(
    page: LibraryPhotoPage,
    visuals: Vec<Option<shadow_catalog::CachedArtifactRecord>>,
    review: &ReviewService,
) -> AnyResult<ffi::FfiLibraryPhotoPage> {
    if page.items.len() != visuals.len() {
        bail!(
            "Library visual selection returned {} slots for {} photos",
            visuals.len(),
            page.items.len()
        );
    }
    let next_cursor = page
        .next_cursor
        .map_or_else(empty_ffi_cursor, ffi_library_cursor);
    Ok(ffi::FfiLibraryPhotoPage {
        items: page
            .items
            .into_iter()
            .zip(visuals)
            .map(|(record, visual)| ffi_library_photo(record, visual, review))
            .collect::<AnyResult<Vec<_>>>()?,
        has_more: !next_cursor.photo_id.is_empty(),
        next_cursor,
    })
}

fn empty_ffi_cursor() -> ffi::FfiLibraryPhotoCursor {
    ffi::FfiLibraryPhotoCursor {
        photo_id: String::new(),
        has_capture_time: false,
        captured_at_unix_seconds: 0,
    }
}

fn ffi_library_cursor(cursor: LibraryPhotoCursor) -> ffi::FfiLibraryPhotoCursor {
    ffi::FfiLibraryPhotoCursor {
        photo_id: cursor.photo_id.to_string(),
        has_capture_time: cursor.captured_at_unix_seconds.is_some(),
        captured_at_unix_seconds: cursor.captured_at_unix_seconds.unwrap_or_default(),
    }
}

#[allow(clippy::too_many_lines)]
fn ffi_library_photo(
    record: LibraryPhotoRecord,
    cached_visual: Option<shadow_catalog::CachedArtifactRecord>,
    review: &ReviewService,
) -> AnyResult<ffi::FfiLibraryPhotoItem> {
    let visual = review.grid_visual(
        record.photo_id,
        record.representation_id,
        record.source,
        cached_visual.as_ref(),
    )?;
    let (visual_handle, visual_role, visual_width, visual_height, has_visual) =
        ffi_grid_visual(visual);
    let facts = record.facts;
    let has_metadata = facts.is_some();
    let (
        has_captured_at,
        captured_at_unix_seconds,
        capture_day,
        camera_make,
        camera_model,
        lens_make,
        lens_model,
        has_aperture,
        aperture_milli,
        has_focal_length,
        focal_length_tenth_mm,
        has_iso_speed,
        iso_speed,
        has_coordinates,
        latitude_e7,
        longitude_e7,
        place_name,
        metadata_indexed_at_ms,
    ) = facts.map_or_else(
        || {
            (
                false,
                0,
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                false,
                0,
                false,
                0,
                false,
                0.0,
                false,
                0,
                0,
                String::new(),
                0,
            )
        },
        |facts| {
            let has_coordinates = facts.latitude_e7.is_some() && facts.longitude_e7.is_some();
            (
                facts.captured_at_unix_seconds.is_some(),
                facts.captured_at_unix_seconds.unwrap_or_default(),
                facts.capture_day,
                facts.camera_make,
                facts.camera_model,
                facts.lens_make,
                facts.lens_model,
                facts.aperture_milli.is_some(),
                facts.aperture_milli.unwrap_or_default(),
                facts.focal_length_tenth_mm.is_some(),
                facts.focal_length_tenth_mm.unwrap_or_default(),
                facts.iso_speed.is_some(),
                facts.iso_speed.unwrap_or_default(),
                has_coordinates,
                facts.latitude_e7.unwrap_or_default(),
                facts.longitude_e7.unwrap_or_default(),
                facts.place_name,
                facts.indexed_at_ms,
            )
        },
    );
    Ok(ffi::FfiLibraryPhotoItem {
        photo_id: record.photo_id.to_string(),
        representation_id: record.representation_id.to_string(),
        title: file_name(&record.location.display_path),
        source_path: record.location.display_path,
        source_byte_len: record.source.byte_len,
        has_source_modified_at: record.source.modified_at_ms.is_some(),
        source_modified_at_ms: record.source.modified_at_ms.unwrap_or_default(),
        visual_handle,
        visual_role,
        visual_width,
        visual_height,
        has_visual,
        has_metadata,
        has_captured_at,
        captured_at_unix_seconds,
        capture_day,
        camera_make,
        camera_model,
        lens_make,
        lens_model,
        has_aperture,
        aperture_milli,
        has_focal_length,
        focal_length_tenth_mm,
        has_iso_speed,
        iso_speed,
        has_coordinates,
        latitude_e7,
        longitude_e7,
        place_name,
        metadata_indexed_at_ms,
        liked: record.state.liked,
        color_label: record.state.color_label,
        library_state_updated_at_ms: record.state.updated_at_ms,
        decision_head_sequence: record.decision.head_sequence,
        decision_flag: ffi_decision_flag(record.decision.flag),
        decision_rating: record.decision.rating,
        has_development_edits: record.has_development_edits,
    })
}

fn ffi_grid_visual(visual: Option<GridVisualPresentation>) -> (String, String, u32, u32, bool) {
    visual.map_or_else(
        || (String::new(), String::new(), 0, 0, false),
        |visual| {
            (
                visual.handle,
                visual.role,
                visual.dimensions.width,
                visual.dimensions.height,
                true,
            )
        },
    )
}

fn ffi_decision_flag(flag: PhotoFlag) -> ffi::FfiDecisionFlag {
    match flag {
        PhotoFlag::Unflagged => ffi::FfiDecisionFlag::Unflagged,
        PhotoFlag::Picked => ffi::FfiDecisionFlag::Picked,
        PhotoFlag::Rejected => ffi::FfiDecisionFlag::Rejected,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use shadow_catalog::{CatalogActor, RegisterAsset};
    use shadow_domain::{AssetLocation, Platform, RepresentationKind};
    use uuid::Uuid;

    #[test]
    fn empty_cursor_starts_a_library_page_but_cannot_carry_capture_time() {
        let start = ffi::FfiLibraryPhotoCursor {
            photo_id: String::new(),
            has_capture_time: false,
            captured_at_unix_seconds: 0,
        };
        assert_eq!(library_cursor_from_ffi(&start).expect("parse start"), None);

        let invalid = ffi::FfiLibraryPhotoCursor {
            photo_id: String::new(),
            has_capture_time: true,
            captured_at_unix_seconds: 1,
        };
        assert!(library_cursor_from_ffi(&invalid).is_err());
    }

    #[test]
    fn filter_keeps_explicit_zero_rating_distinct_from_no_rating_filter() {
        let filter = ffi::FfiLibraryPhotoFilter {
            has_minimum_rating: true,
            minimum_rating: 0,
            ..neutral_ffi_filter()
        };
        assert_eq!(
            library_filter_from_ffi(&filter)
                .expect("parse explicit zero rating")
                .minimum_rating,
            Some(0)
        );
        assert_eq!(
            library_filter_from_ffi(&neutral_ffi_filter())
                .expect("parse neutral filter")
                .minimum_rating,
            None
        );
    }

    #[test]
    fn filter_keeps_explicit_edited_state_distinct_from_no_edit_filter() {
        let filter = ffi::FfiLibraryPhotoFilter {
            has_development_edits: true,
            development_edits: false,
            ..neutral_ffi_filter()
        };
        assert_eq!(
            library_filter_from_ffi(&filter)
                .expect("parse explicit unedited filter")
                .has_development_edits,
            Some(false)
        );
        assert_eq!(
            library_filter_from_ffi(&neutral_ffi_filter())
                .expect("parse neutral filter")
                .has_development_edits,
            None
        );
    }

    #[test]
    fn album_service_keeps_manual_membership_and_smart_queries_separate() {
        let root =
            std::env::temp_dir().join(format!("shadow-library-service-album-{}", Uuid::now_v7()));
        std::fs::create_dir_all(&root).expect("create catalog fixture root");
        let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
        let service = LibraryService::new(actor.handle());
        let registered = service
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/library-service-album.dng".to_vec(),
                    "/photos/library-service-album.dng",
                ),
                byte_len: 42,
                modified_at_ms: Some(100),
                now_ms: 1,
            })
            .expect("register Library photo");
        let manual = service
            .create_manual_album("Travel", 2)
            .expect("create manual album");
        service
            .add_photo_to_manual_album(
                &manual.id.to_string(),
                &registered.photo_id.to_string(),
                0,
                3,
            )
            .expect("add manual membership");
        assert_eq!(
            service
                .albums_for_photo(&registered.photo_id.to_string())
                .expect("read manual membership"),
            vec![manual.clone()]
        );
        let renamed = service
            .rename_album(&manual.id.to_string(), "Travel 2026", 4)
            .expect("rename manual album");
        assert_eq!(renamed.name, "Travel 2026");

        let query = SmartAlbumQueryV1::new(LibraryPhotoFilter::default())
            .expect("create all-photos smart query");
        let smart = service
            .create_smart_album("All photos", &query, 5)
            .expect("create smart album");
        assert_eq!(
            service
                .smart_album_photo_count(&smart.id.to_string())
                .expect("count smart album"),
            1
        );
        let replaced = service
            .replace_smart_album_query(&smart.id.to_string(), &query, 6)
            .expect("replace smart album query");
        assert_eq!(replaced.updated_at_ms, 6);
        assert!(
            service
                .remove_photo_from_manual_album(
                    &manual.id.to_string(),
                    &registered.photo_id.to_string(),
                )
                .expect("remove manual membership")
        );
        assert!(
            service
                .delete_album(&manual.id.to_string())
                .expect("delete manual album")
        );
        assert_eq!(
            service.albums().expect("list remaining albums"),
            vec![replaced]
        );
        assert!(library_album_id_from_text("").is_err());
        assert!(library_photo_id_from_text("not-a-photo-id").is_err());
        actor.shutdown().expect("shutdown catalog actor");
        std::fs::remove_dir_all(root).expect("remove catalog fixture root");
    }

    fn neutral_ffi_filter() -> ffi::FfiLibraryPhotoFilter {
        ffi::FfiLibraryPhotoFilter {
            has_capture_start: false,
            capture_start_unix_seconds: 0,
            has_capture_end: false,
            capture_end_unix_seconds: 0,
            capture_month: String::new(),
            camera_key: String::new(),
            lens_key: String::new(),
            has_aperture_minimum: false,
            aperture_minimum_milli: 0,
            has_aperture_maximum: false,
            aperture_maximum_milli: 0,
            has_liked: false,
            liked: false,
            color_label: String::new(),
            flag: ffi::FfiLibraryFlagFilter::Any,
            has_minimum_rating: false,
            minimum_rating: 0,
            has_development_edits: false,
            development_edits: false,
            album_id: String::new(),
        }
    }
}

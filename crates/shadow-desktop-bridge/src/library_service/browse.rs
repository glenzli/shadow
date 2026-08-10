//! Photo-first Library paging, facets, and visual presentation.

use anyhow::{Result as AnyResult, bail};
use shadow_catalog::{CachedArtifactRecord, LibraryPhotoPage, LibraryPhotoRecord};
use shadow_core::native_path_from_location;
use shadow_domain::PhotoFlag;

use crate::{
    ffi,
    review_service::{GridVisualPresentation, ReviewService, file_name},
};

use super::{
    LibraryService,
    query_contract::{
        empty_ffi_cursor, ffi_library_cursor, ffi_library_facet_page, library_cursor_from_ffi,
        library_facet_cursor_from_ffi, library_facet_kind_from_ffi, library_filter_from_ffi,
        library_order_from_ffi,
    },
};

impl LibraryService {
    /// Reads one bounded, keyset-paginated photo-first page.
    pub(crate) fn photo_page(
        &self,
        review: &ReviewService,
        ffi_filter: &ffi::FfiLibraryPhotoFilter,
        ffi_order: ffi::FfiLibraryPhotoOrder,
        ffi_cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        let filter = library_filter_from_ffi(ffi_filter)?;
        let order = library_order_from_ffi(ffi_order)?;
        let cursor = library_cursor_from_ffi(order, ffi_cursor)?;
        let page = self.catalog.library_photo_page(
            &filter,
            order,
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

    pub(super) fn present_photo_page(
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

fn ffi_library_page(
    page: LibraryPhotoPage,
    visuals: Vec<Option<CachedArtifactRecord>>,
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
        .as_ref()
        .map_or_else(empty_ffi_cursor, ffi_library_cursor);
    Ok(ffi::FfiLibraryPhotoPage {
        items: page
            .items
            .into_iter()
            .zip(visuals)
            .map(|(record, visual)| ffi_library_photo(record, visual.as_ref(), review))
            .collect::<AnyResult<Vec<_>>>()?,
        has_more: !next_cursor.photo_id.is_empty(),
        next_cursor,
    })
}

#[allow(clippy::too_many_lines)]
fn ffi_library_photo(
    record: LibraryPhotoRecord,
    cached_visual: Option<&CachedArtifactRecord>,
    review: &ReviewService,
) -> AnyResult<ffi::FfiLibraryPhotoItem> {
    let source_available = native_path_from_location(&record.location)
        .ok()
        .and_then(|path| std::fs::metadata(path).ok())
        .is_some_and(|metadata| metadata.is_file());
    let visual = review.grid_visual(
        record.photo_id,
        record.representation_id,
        record.source,
        cached_visual,
    )?;
    let (visual_handle, visual_role, visual_width, visual_height, has_visual) =
        ffi_grid_visual(visual);
    let facts = record.facts;
    let (resolved_country_name, resolved_locality_label, resolved_place_name) =
        record.resolved_place.map_or_else(
            || (String::new(), String::new(), String::new()),
            |place| (place.country_name, place.locality_label, place.display_name),
        );
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
        representation_count: record.representation_count,
        source_location_count: record.source_location_count,
        has_raw_representation: record.has_raw_representation,
        has_raster_representation: record.has_raster_representation,
        location_id: record.location_id.to_string(),
        title: file_name(&record.location.display_path),
        source_path: record.location.display_path,
        source_available,
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
        resolved_country_name,
        resolved_locality_label,
        resolved_place_name,
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

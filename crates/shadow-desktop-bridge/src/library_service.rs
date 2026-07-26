//! Photo-first Library queries for the desktop bridge.
//!
//! This service deliberately owns only the conversion between the stable,
//! path-independent Catalog Library projection and the compact CXX DTO used by
//! the desktop shell.  Review remains a separate, session-local visual and
//! comparison service: replacing its grid query must not change the evidence
//! or signed-preview contract.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    CatalogHandle, LibraryApertureRange, LibraryDateRange, LibraryPhotoCursor, LibraryPhotoFilter,
    LibraryPhotoPage, LibraryPhotoRecord, SetPhotoLibraryState,
};
use shadow_domain::{CollectionId, PhotoFlag, PhotoId};

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

    /// Counts a settled filter. The desktop grid should not call this during
    /// scroll; it is intentionally separate from [`Self::photo_page`].
    pub(crate) fn photo_count(&self, ffi_filter: &ffi::FfiLibraryPhotoFilter) -> AnyResult<u64> {
        let filter = library_filter_from_ffi(ffi_filter)?;
        Ok(self.catalog.library_photo_count(&filter)?)
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
        .map(|value| {
            value
                .parse::<CollectionId>()
                .with_context(|| format!("parse Library album id {value}"))
        })
        .transpose()?;

    Ok(LibraryPhotoFilter {
        capture_time,
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

    fn neutral_ffi_filter() -> ffi::FfiLibraryPhotoFilter {
        ffi::FfiLibraryPhotoFilter {
            has_capture_start: false,
            capture_start_unix_seconds: 0,
            has_capture_end: false,
            capture_end_unix_seconds: 0,
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

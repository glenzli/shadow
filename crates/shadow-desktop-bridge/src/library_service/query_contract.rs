//! Typed conversion for Library filters, facets, and pagination cursors.
//!
//! This is the narrow contract shared by ordinary browsing and smart-album
//! queries. It owns no Catalog lifecycle or photo presentation behavior.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    LibraryApertureRange, LibraryDateRange, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage,
    LibraryLivingPlaceRule, LibraryPhotoCursor, LibraryPhotoCursorValue, LibraryPhotoFilter,
    LibraryPhotoOrder, SmartAlbumQueryV1,
};
use shadow_domain::{CollectionId, KeywordId, PhotoFlag, PhotoId};

use crate::ffi;

pub(super) fn library_filter_from_ffi(
    filter: &ffi::FfiLibraryPhotoFilter,
) -> AnyResult<LibraryPhotoFilter> {
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
        chinese_lunar_month: filter
            .has_chinese_lunar_month
            .then_some(filter.chinese_lunar_month),
        chinese_lunar_day: filter
            .has_chinese_lunar_day
            .then_some(filter.chinese_lunar_day),
        chinese_lunar_is_leap_month: filter
            .has_chinese_lunar_is_leap_month
            .then_some(filter.chinese_lunar_is_leap_month),
        camera_key: optional_filter_text(&filter.camera_key),
        lens_key: optional_filter_text(&filter.lens_key),
        country_key: optional_filter_text(&filter.country_key),
        locality_key: optional_filter_text(&filter.locality_key),
        living_place_rules: filter
            .living_place_rules
            .iter()
            .map(|rule| LibraryLivingPlaceRule {
                locality_key: rule.locality_key.trim().to_owned(),
                start_month: optional_filter_text(&rule.start_month),
                end_month: optional_filter_text(&rule.end_month),
            })
            .collect(),
        include_living_place_rules: filter.include_living_place_rules,
        aperture,
        liked: filter.has_liked.then_some(filter.liked),
        color_label: optional_filter_text(&filter.color_label),
        flag,
        minimum_rating: filter.has_minimum_rating.then_some(filter.minimum_rating),
        has_development_edits: filter
            .has_development_edits
            .then_some(filter.development_edits),
        album_id,
        keyword_ids_all: keyword_ids_from_ffi(&filter.keyword_ids_all)?,
        excluded_keyword_ids_any: keyword_ids_from_ffi(&filter.excluded_keyword_ids_any)?,
    })
}

fn optional_filter_text(value: &str) -> Option<String> {
    let trimmed = value.trim();
    (!trimmed.is_empty()).then(|| trimmed.to_owned())
}

fn keyword_ids_from_ffi(keyword_ids: &[String]) -> AnyResult<Vec<KeywordId>> {
    keyword_ids
        .iter()
        .map(|keyword_id| {
            let keyword_id = keyword_id.trim();
            if keyword_id.is_empty() {
                bail!("Library keyword filter ids must not be empty");
            }
            keyword_id
                .parse::<KeywordId>()
                .with_context(|| format!("parse Library keyword id {keyword_id}"))
        })
        .collect()
}

pub(super) fn smart_album_query_from_ffi(
    filter: &ffi::FfiLibraryPhotoFilter,
) -> AnyResult<SmartAlbumQueryV1> {
    Ok(SmartAlbumQueryV1::new(library_filter_from_ffi(filter)?)?)
}

pub(super) fn library_album_id_from_text(album_id: &str) -> AnyResult<CollectionId> {
    let album_id = album_id.trim();
    if album_id.is_empty() {
        bail!("Library album id is required");
    }
    album_id
        .parse::<CollectionId>()
        .with_context(|| format!("parse Library album id {album_id}"))
}

pub(super) fn empty_ffi_library_filter() -> ffi::FfiLibraryPhotoFilter {
    ffi_library_filter(LibraryPhotoFilter::default())
}

pub(super) fn ffi_library_filter(filter: LibraryPhotoFilter) -> ffi::FfiLibraryPhotoFilter {
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
        has_chinese_lunar_month: filter.chinese_lunar_month.is_some(),
        chinese_lunar_month: filter.chinese_lunar_month.unwrap_or_default(),
        has_chinese_lunar_day: filter.chinese_lunar_day.is_some(),
        chinese_lunar_day: filter.chinese_lunar_day.unwrap_or_default(),
        has_chinese_lunar_is_leap_month: filter.chinese_lunar_is_leap_month.is_some(),
        chinese_lunar_is_leap_month: filter.chinese_lunar_is_leap_month.unwrap_or_default(),
        camera_key: filter.camera_key.unwrap_or_default(),
        lens_key: filter.lens_key.unwrap_or_default(),
        country_key: filter.country_key.unwrap_or_default(),
        locality_key: filter.locality_key.unwrap_or_default(),
        living_place_rules: filter
            .living_place_rules
            .into_iter()
            .map(|rule| ffi::FfiLibraryLivingPlaceRule {
                locality_key: rule.locality_key,
                start_month: rule.start_month.unwrap_or_default(),
                end_month: rule.end_month.unwrap_or_default(),
            })
            .collect(),
        include_living_place_rules: filter.include_living_place_rules,
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
        keyword_ids_all: filter
            .keyword_ids_all
            .into_iter()
            .map(|id| id.to_string())
            .collect(),
        excluded_keyword_ids_any: filter
            .excluded_keyword_ids_any
            .into_iter()
            .map(|id| id.to_string())
            .collect(),
    }
}

pub(super) fn library_facet_kind_from_ffi(
    kind: ffi::FfiLibraryFacetKind,
) -> AnyResult<LibraryFacetKind> {
    match kind {
        ffi::FfiLibraryFacetKind::CaptureMonth => Ok(LibraryFacetKind::CaptureMonth),
        ffi::FfiLibraryFacetKind::Camera => Ok(LibraryFacetKind::Camera),
        ffi::FfiLibraryFacetKind::Lens => Ok(LibraryFacetKind::Lens),
        ffi::FfiLibraryFacetKind::Country => Ok(LibraryFacetKind::Country),
        ffi::FfiLibraryFacetKind::City => Ok(LibraryFacetKind::City),
        _ => bail!("unknown Library facet kind"),
    }
}

pub(super) fn library_facet_cursor_from_ffi(
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

pub(super) fn ffi_library_facet_page(page: LibraryFacetPage) -> ffi::FfiLibraryFacetPage {
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

pub(super) fn library_cursor_from_ffi(
    order: LibraryPhotoOrder,
    cursor: &ffi::FfiLibraryPhotoCursor,
) -> AnyResult<Option<LibraryPhotoCursor>> {
    if cursor.photo_id.is_empty() {
        if cursor.has_capture_time || !cursor.file_name.is_empty() {
            bail!("Library cursor sort value requires a photo id");
        }
        return Ok(None);
    }
    let photo_id = cursor
        .photo_id
        .parse::<PhotoId>()
        .with_context(|| format!("parse Library cursor photo id {}", cursor.photo_id))?;
    let value = match order {
        LibraryPhotoOrder::CaptureTimeDescending | LibraryPhotoOrder::CaptureTimeAscending => {
            if !cursor.file_name.is_empty() {
                bail!("capture-time Library cursor cannot carry a file name");
            }
            LibraryPhotoCursorValue::CaptureTime(
                cursor
                    .has_capture_time
                    .then_some(cursor.captured_at_unix_seconds),
            )
        }
        LibraryPhotoOrder::FileNameAscending | LibraryPhotoOrder::FileNameDescending => {
            if cursor.has_capture_time || cursor.file_name.is_empty() {
                bail!("file-name Library cursor requires only a file name");
            }
            LibraryPhotoCursorValue::FileName(cursor.file_name.clone())
        }
    };
    Ok(Some(LibraryPhotoCursor { value, photo_id }))
}

pub(super) fn empty_ffi_cursor() -> ffi::FfiLibraryPhotoCursor {
    ffi::FfiLibraryPhotoCursor {
        photo_id: String::new(),
        has_capture_time: false,
        captured_at_unix_seconds: 0,
        file_name: String::new(),
    }
}

pub(super) fn ffi_library_cursor(cursor: &LibraryPhotoCursor) -> ffi::FfiLibraryPhotoCursor {
    let (has_capture_time, captured_at_unix_seconds, file_name) = match &cursor.value {
        LibraryPhotoCursorValue::CaptureTime(captured_at) => (
            captured_at.is_some(),
            captured_at.unwrap_or_default(),
            String::new(),
        ),
        LibraryPhotoCursorValue::FileName(file_name) => (false, 0, file_name.clone()),
    };
    ffi::FfiLibraryPhotoCursor {
        photo_id: cursor.photo_id.to_string(),
        has_capture_time,
        captured_at_unix_seconds,
        file_name,
    }
}

pub(super) fn library_order_from_ffi(
    order: ffi::FfiLibraryPhotoOrder,
) -> AnyResult<LibraryPhotoOrder> {
    match order {
        ffi::FfiLibraryPhotoOrder::CaptureTimeDescending => {
            Ok(LibraryPhotoOrder::CaptureTimeDescending)
        }
        ffi::FfiLibraryPhotoOrder::CaptureTimeAscending => {
            Ok(LibraryPhotoOrder::CaptureTimeAscending)
        }
        ffi::FfiLibraryPhotoOrder::FileNameAscending => Ok(LibraryPhotoOrder::FileNameAscending),
        ffi::FfiLibraryPhotoOrder::FileNameDescending => Ok(LibraryPhotoOrder::FileNameDescending),
        _ => bail!("unknown Library photo order"),
    }
}

#[cfg(test)]
mod tests;

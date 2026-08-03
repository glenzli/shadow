use crate::ffi;
use shadow_catalog::LibraryPhotoOrder;
use shadow_domain::{EntityId, KeywordId, PhotoId};

use super::{ffi_library_filter, library_cursor_from_ffi, library_filter_from_ffi};

#[test]
fn empty_cursor_starts_a_library_page_but_cannot_carry_capture_time() {
    let start = ffi::FfiLibraryPhotoCursor {
        photo_id: String::new(),
        has_capture_time: false,
        captured_at_unix_seconds: 0,
        file_name: String::new(),
    };
    assert_eq!(
        library_cursor_from_ffi(LibraryPhotoOrder::default(), &start).expect("parse start"),
        None
    );

    let invalid = ffi::FfiLibraryPhotoCursor {
        photo_id: String::new(),
        has_capture_time: true,
        captured_at_unix_seconds: 1,
        file_name: String::new(),
    };
    assert!(library_cursor_from_ffi(LibraryPhotoOrder::default(), &invalid).is_err());
}

#[test]
fn cursor_sort_value_must_match_the_requested_order() {
    let photo_id = PhotoId::new_v7().to_string();
    let name_cursor = ffi::FfiLibraryPhotoCursor {
        photo_id,
        has_capture_time: false,
        captured_at_unix_seconds: 0,
        file_name: "image.nef".into(),
    };
    assert!(library_cursor_from_ffi(LibraryPhotoOrder::FileNameAscending, &name_cursor).is_ok());
    assert!(
        library_cursor_from_ffi(LibraryPhotoOrder::CaptureTimeDescending, &name_cursor).is_err()
    );
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
fn keyword_filter_ids_round_trip_without_becoming_untyped_text() {
    let required = KeywordId::new_v7();
    let excluded = KeywordId::new_v7();
    let filter = ffi::FfiLibraryPhotoFilter {
        keyword_ids_all: vec![required.to_string()],
        excluded_keyword_ids_any: vec![excluded.to_string()],
        ..neutral_ffi_filter()
    };
    let typed = library_filter_from_ffi(&filter).expect("parse keyword ids");
    assert_eq!(typed.keyword_ids_all, vec![required]);
    assert_eq!(typed.excluded_keyword_ids_any, vec![excluded]);
    let projected = ffi_library_filter(typed);
    assert_eq!(projected.keyword_ids_all, vec![required.to_string()]);
    assert_eq!(
        projected.excluded_keyword_ids_any,
        vec![excluded.to_string()]
    );
}

#[test]
fn structured_place_filter_keys_round_trip_without_using_display_labels() {
    let filter = ffi::FfiLibraryPhotoFilter {
        country_key: " CN ".into(),
        locality_key: "cn\u{1f}shanghai\u{1f}shanghai".into(),
        excluded_locality_key: "cn\u{1f}beijing\u{1f}beijing".into(),
        ..neutral_ffi_filter()
    };
    let typed = library_filter_from_ffi(&filter).expect("parse structured place keys");
    assert_eq!(typed.country_key.as_deref(), Some("CN"));
    assert_eq!(
        typed.locality_key.as_deref(),
        Some("cn\u{1f}shanghai\u{1f}shanghai")
    );
    assert_eq!(
        typed.excluded_locality_key.as_deref(),
        Some("cn\u{1f}beijing\u{1f}beijing")
    );
    let projected = ffi_library_filter(typed);
    assert_eq!(projected.country_key, "CN");
    assert_eq!(projected.locality_key, "cn\u{1f}shanghai\u{1f}shanghai");
    assert_eq!(
        projected.excluded_locality_key,
        "cn\u{1f}beijing\u{1f}beijing"
    );
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
        country_key: String::new(),
        locality_key: String::new(),
        excluded_locality_key: String::new(),
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
        keyword_ids_all: Vec::new(),
        excluded_keyword_ids_any: Vec::new(),
    }
}

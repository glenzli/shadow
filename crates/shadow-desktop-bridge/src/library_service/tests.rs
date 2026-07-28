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

use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    LibraryFacetKind, LibraryPhotoFacts, LibraryPhotoFilter, RegisterAsset,
    RepresentationFingerprint,
};

use super::CatalogActor;

#[test]
fn actor_routes_photo_pages_counts_and_bounded_facets() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/library-page.dng".to_vec(),
                "/photos/library-page.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register Library photo");
    handle
        .upsert_photo_library_facts(&LibraryPhotoFacts {
            photo_id: registered.photo_id,
            captured_at_unix_seconds: Some(1_700_000_000),
            capture_day: "2023-11-14".into(),
            camera_make: "NIKON CORPORATION".into(),
            camera_model: "NIKON Z 8".into(),
            lens_make: String::new(),
            lens_model: String::new(),
            aperture_milli: None,
            focal_length_tenth_mm: None,
            iso_speed: None,
            latitude_e7: None,
            longitude_e7: None,
            place_name: String::new(),
            indexed_representation_id: Some(registered.representation_id),
            indexed_source: Some(RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            }),
            indexed_at_ms: 1_700_000_000_010,
        })
        .expect("index Library facts through actor");

    let page = handle
        .library_photo_page(&LibraryPhotoFilter::default(), None, 16)
        .expect("page Library through actor");
    assert_eq!(page.items[0].photo_id, registered.photo_id);
    assert_eq!(
        page.items[0].location.display_path,
        "/photos/library-page.dng"
    );
    assert_eq!(
        handle
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count Library through actor"),
        1
    );
    let cameras = handle
        .library_facet_page(
            &LibraryPhotoFilter::default(),
            LibraryFacetKind::Camera,
            None,
            16,
        )
        .expect("read camera facets through actor");
    assert_eq!(cameras.items.len(), 1);
    assert_eq!(cameras.items[0].photo_count, 1);
    assert!(cameras.next_cursor.is_none());
    actor.shutdown().expect("shutdown actor");
}

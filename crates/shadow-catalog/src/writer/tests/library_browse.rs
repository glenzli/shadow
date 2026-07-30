use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    LibraryFacetKind, LibraryMapGrid, LibraryMapViewport, LibraryPhotoFacts, LibraryPhotoFilter,
    RegisterAsset, RepresentationFingerprint,
};

use crate::writer::CatalogActor;

#[test]
fn actor_routes_photo_pages_counts_facets_and_map_clusters() {
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
            latitude_e7: Some(312_304_000),
            longitude_e7: Some(1_212_473_000),
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
    let map = handle
        .library_map_snapshot(
            &LibraryPhotoFilter::default(),
            LibraryMapViewport {
                south_latitude_e7: 300_000_000,
                west_longitude_e7: 1_200_000_000,
                north_latitude_e7: 320_000_000,
                east_longitude_e7: 1_220_000_000,
            },
            LibraryMapGrid {
                columns: 8,
                rows: 8,
            },
        )
        .expect("read map clusters through actor");
    assert_eq!(map.photo_count, 1);
    assert_eq!(map.clusters[0].single_photo_id, Some(registered.photo_id));
    actor.shutdown().expect("shutdown actor");
}

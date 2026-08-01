use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    LibraryPhotoFacts, RecordLibraryPlaceResolution, RecordLibraryPlaceResolutionStatus,
    RegisterAsset, RepresentationFingerprint,
};

use crate::writer::CatalogActor;

#[test]
fn actor_routes_place_candidates_guarded_writes_and_reads() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/place.dng".to_vec(),
                "/photos/place.dng",
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
            camera_make: "Nikon".into(),
            camera_model: "Z 8".into(),
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
        .expect("index coordinates through actor");

    let candidates = handle
        .library_place_resolution_candidates(16)
        .expect("read candidates through actor");
    assert_eq!(candidates.len(), 1);
    assert_eq!(candidates[0].photo_count, 1);

    assert_eq!(
        handle
            .record_library_place_resolution(&RecordLibraryPlaceResolution {
                latitude_e7: 312_304_000,
                longitude_e7: 1_212_473_000,
                country_code: "CN".into(),
                country_name: "China".into(),
                administrative_area: "Shanghai".into(),
                locality: "Shanghai".into(),
                display_name: "Shanghai, China".into(),
                provider_id: "test".into(),
                provider_version: "1".into(),
                locale: "en".into(),
                resolved_at_ms: 1_700_000_000_020,
            })
            .expect("record place through actor"),
        RecordLibraryPlaceResolutionStatus::Recorded
    );
    assert_eq!(
        handle
            .library_place_resolution(312_304_000, 1_212_473_000)
            .expect("read place through actor")
            .expect("place exists")
            .locality_label,
        "Shanghai · China"
    );
    assert!(
        handle
            .library_place_resolution_candidates(16)
            .expect("read resolved queue")
            .is_empty()
    );
    actor.shutdown().expect("shutdown actor");
}

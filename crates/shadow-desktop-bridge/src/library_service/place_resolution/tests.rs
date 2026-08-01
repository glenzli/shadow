use shadow_catalog::{CatalogActor, LibraryPhotoFacts, RegisterAsset, RepresentationFingerprint};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};
use uuid::Uuid;

use crate::ffi;

use super::LibraryService;

#[test]
fn service_projects_bounded_candidates_and_records_provider_neutral_results() {
    let root =
        std::env::temp_dir().join(format!("shadow-library-place-service-{}", Uuid::now_v7()));
    std::fs::create_dir_all(&root).expect("create catalog fixture root");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let service = LibraryService::new(actor.handle());
    let registered = service
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/place-service.dng".to_vec(),
                "/photos/place-service.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register Library photo");
    service
        .catalog
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
            indexed_at_ms: 2,
        })
        .expect("index coordinates");

    let candidates = service
        .place_resolution_candidates(16)
        .expect("read bridge candidates");
    assert_eq!(candidates.len(), 1);
    assert_eq!(candidates[0].photo_count, 1);
    assert_eq!(
        service
            .record_place_resolution(
                &ffi::FfiLibraryPlaceResolutionResult {
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
                },
                3,
            )
            .expect("record bridge result"),
        ffi::FfiRecordLibraryPlaceResolutionStatus::Recorded
    );
    assert!(
        service
            .place_resolution_candidates(16)
            .expect("read resolved queue")
            .is_empty()
    );

    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove catalog fixture");
}

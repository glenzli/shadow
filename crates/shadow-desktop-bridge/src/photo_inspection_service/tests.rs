use shadow_catalog::{
    CatalogActor, LibraryPhotoFacts, RecordLibraryPlaceResolution, RegisterAsset,
    RepresentationFingerprint,
};
use shadow_domain::{
    AssetLocation, EntityId, PhotoId, Platform, RepresentationId, RepresentationKind,
};

use super::PhotoInspectionService;

#[test]
fn service_preserves_exact_identity_and_reports_normal_absence() {
    let root = fixture_root("exact");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let registered = actor
        .handle()
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/selected.jpg".to_vec(),
                "/photos/selected.jpg",
            ),
            byte_len: 1_024,
            modified_at_ms: Some(55),
            now_ms: 100,
        })
        .expect("register exact photo");
    let service = PhotoInspectionService::new(actor.handle());

    let available = service
        .inspect(
            &registered.photo_id.to_string(),
            &registered.representation_id.to_string(),
        )
        .expect("inspect exact pair");
    assert!(available.available);
    assert_eq!(available.photo_id, registered.photo_id.to_string());
    assert_eq!(
        available.representation_id,
        registered.representation_id.to_string()
    );
    assert_eq!(available.source_path, "/photos/selected.jpg");
    assert_eq!(available.source_byte_len, 1_024);
    assert!(available.has_source_modified_at);
    assert_eq!(available.source_modified_at_ms, 55);

    let absent = service
        .inspect(
            &PhotoId::new_v7().to_string(),
            &registered.representation_id.to_string(),
        )
        .expect("mismatched pair is normal absence");
    assert!(!absent.available);
    assert_eq!(
        absent.representation_id,
        registered.representation_id.to_string()
    );
    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove exact inspection fixture");
}

#[test]
fn service_rejects_malformed_identity_before_querying_catalog() {
    let root = fixture_root("invalid");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let service = PhotoInspectionService::new(actor.handle());

    assert!(
        service
            .inspect("not-a-photo-id", &RepresentationId::new_v7().to_string())
            .expect_err("malformed photo id must fail")
            .to_string()
            .contains("parse selected photo id")
    );
    assert!(
        service
            .inspect(&PhotoId::new_v7().to_string(), "not-a-representation-id")
            .expect_err("malformed representation id must fail")
            .to_string()
            .contains("parse selected representation id")
    );
    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove invalid inspection fixture");
}

#[test]
fn service_projects_the_coordinate_bound_resolved_place_without_overwriting_metadata() {
    let root = fixture_root("resolved-place");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/yellowstone.nef".to_vec(),
                "/photos/yellowstone.nef",
            ),
            byte_len: 4_096,
            modified_at_ms: Some(100),
            now_ms: 200,
        })
        .expect("register geotagged photo");
    handle
        .upsert_photo_library_facts(&LibraryPhotoFacts {
            photo_id: registered.photo_id,
            captured_at_unix_seconds: None,
            capture_day: String::new(),
            camera_make: String::new(),
            camera_model: String::new(),
            lens_make: String::new(),
            lens_model: String::new(),
            aperture_milli: None,
            focal_length_tenth_mm: None,
            iso_speed: None,
            latitude_e7: Some(446_198_000),
            longitude_e7: Some(-1_104_255_267),
            place_name: String::new(),
            indexed_representation_id: Some(registered.representation_id),
            indexed_source: Some(RepresentationFingerprint {
                byte_len: 4_096,
                modified_at_ms: Some(100),
            }),
            indexed_at_ms: 210,
        })
        .expect("index geotagged facts");
    handle
        .record_library_place_resolution(&RecordLibraryPlaceResolution {
            latitude_e7: 446_198_000,
            longitude_e7: -1_104_255_267,
            country_code: "US".into(),
            country_name: "United States".into(),
            administrative_area: "Wyoming".into(),
            locality: "Yellowstone National Park".into(),
            display_name: "Yellowstone National Park, Wyoming, United States".into(),
            provider_id: "google-geocoding".into(),
            provider_version: "v1".into(),
            locale: "en-US".into(),
            resolved_at_ms: 220,
        })
        .expect("record resolved place");

    let inspection = PhotoInspectionService::new(handle)
        .inspect(
            &registered.photo_id.to_string(),
            &registered.representation_id.to_string(),
        )
        .expect("inspect resolved place");
    assert!(inspection.has_coordinates);
    assert!(inspection.place_name.is_empty());
    assert_eq!(
        inspection.resolved_place_name,
        "Yellowstone National Park · Wyoming · United States"
    );
    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove resolved-place inspection fixture");
}

fn fixture_root(label: &str) -> std::path::PathBuf {
    let root = std::env::temp_dir().join(format!(
        "shadow-photo-inspection-{label}-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create photo-inspection fixture");
    root
}

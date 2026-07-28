use super::asset_registration_fixture::register;
use crate::{Catalog, LibraryPhotoFacts, RepresentationFingerprint};

#[test]
fn facts_are_indexed_with_their_source_provenance() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = register(&mut catalog, "/archive/facts.nef");
    let facts = LibraryPhotoFacts {
        photo_id: registered.photo_id,
        captured_at_unix_seconds: Some(1_700_000_000),
        capture_day: "2023-11-14".into(),
        camera_make: "NIKON CORPORATION".into(),
        camera_model: "NIKON Z 8".into(),
        lens_make: "Nikon".into(),
        lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
        aperture_milli: Some(4_000),
        focal_length_tenth_mm: Some(240),
        iso_speed: Some(800.0),
        latitude_e7: Some(399_000_000),
        longitude_e7: Some(1_164_000_000),
        place_name: "Beijing".into(),
        indexed_representation_id: Some(registered.representation_id),
        indexed_source: Some(RepresentationFingerprint {
            byte_len: 100,
            modified_at_ms: Some(10),
        }),
        indexed_at_ms: 100,
    };
    catalog
        .upsert_photo_library_facts(&facts)
        .expect("record facts");
    assert_eq!(
        catalog
            .photo_library_facts(registered.photo_id)
            .expect("read facts"),
        Some(facts)
    );
}

use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{LibraryPhotoFacts, RegisterAsset, RepresentationFingerprint};

use crate::writer::CatalogActor;

#[test]
fn actor_round_trips_filterable_library_facts_with_source_provenance() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/archive/library-facts.nef".to_vec(),
                "/archive/library-facts.nef",
            ),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register source through actor");
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
        indexed_at_ms: 30,
    };

    handle
        .upsert_photo_library_facts(&facts)
        .expect("persist facts through actor");
    assert_eq!(
        handle
            .photo_library_facts(registered.photo_id)
            .expect("read facts through actor"),
        Some(facts)
    );
    actor.shutdown().expect("shutdown actor");
}

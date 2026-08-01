use crate::{
    Catalog, LibraryPhotoFacts, RecordLibraryPlaceResolution, RecordLibraryPlaceResolutionStatus,
    RegisterAsset, RepresentationFingerprint,
};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};

fn register_with_coordinates(
    catalog: &mut Catalog,
    path: &str,
    latitude_e7: i32,
    longitude_e7: i32,
) -> crate::RegisteredAsset {
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register source");
    catalog
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
            latitude_e7: Some(latitude_e7),
            longitude_e7: Some(longitude_e7),
            place_name: String::new(),
            indexed_representation_id: Some(registered.representation_id),
            indexed_source: Some(RepresentationFingerprint {
                byte_len: 100,
                modified_at_ms: Some(10),
            }),
            indexed_at_ms: 100,
        })
        .expect("index coordinates");
    registered
}

fn shanghai_record(latitude_e7: i32, longitude_e7: i32) -> RecordLibraryPlaceResolution {
    RecordLibraryPlaceResolution {
        latitude_e7,
        longitude_e7,
        country_code: " cn ".into(),
        country_name: " China ".into(),
        administrative_area: " Shanghai ".into(),
        locality: " Shanghai ".into(),
        display_name: " Shanghai, China ".into(),
        provider_id: " mapkit ".into(),
        provider_version: " 1 ".into(),
        locale: " zh-CN ".into(),
        resolved_at_ms: 500,
    }
}

#[test]
fn candidates_group_exact_coordinates_and_record_normalized_place_facts() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    register_with_coordinates(&mut catalog, "/places/one.nef", 312_304_000, 1_212_473_000);
    register_with_coordinates(&mut catalog, "/places/two.nef", 312_304_000, 1_212_473_000);
    register_with_coordinates(
        &mut catalog,
        "/places/three.nef",
        399_042_000,
        1_164_074_000,
    );

    let candidates = catalog
        .library_place_resolution_candidates(256)
        .expect("read candidates");
    assert_eq!(candidates.len(), 2);
    assert_eq!(candidates[0].latitude_e7, 312_304_000);
    assert_eq!(candidates[0].photo_count, 2);

    assert_eq!(
        catalog
            .record_library_place_resolution(&shanghai_record(312_304_000, 1_212_473_000))
            .expect("record Shanghai"),
        RecordLibraryPlaceResolutionStatus::Recorded
    );
    assert_eq!(
        catalog
            .library_place_resolution_candidates(256)
            .expect("read remaining candidates")
            .len(),
        1
    );

    let place = catalog
        .library_place_resolution(312_304_000, 1_212_473_000)
        .expect("read Shanghai")
        .expect("Shanghai exists");
    assert_eq!(place.country_code, "CN");
    assert_eq!(place.country_key, "cn");
    assert_eq!(place.locality_key, "cn\u{1f}shanghai\u{1f}shanghai");
    assert_eq!(place.locality_label, "Shanghai · China");
    assert_eq!(place.provider_id, "mapkit");
}

#[test]
fn late_provider_result_is_rejected_after_effective_coordinates_change() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = register_with_coordinates(
        &mut catalog,
        "/places/moved.nef",
        312_304_000,
        1_212_473_000,
    );
    let mut changed = catalog
        .photo_library_facts(registered.photo_id)
        .expect("read facts")
        .expect("facts exist");
    changed.latitude_e7 = Some(399_042_000);
    changed.longitude_e7 = Some(1_164_074_000);
    changed.indexed_at_ms += 1;
    catalog
        .upsert_photo_library_facts(&changed)
        .expect("update coordinates");

    assert_eq!(
        catalog
            .record_library_place_resolution(&shanghai_record(312_304_000, 1_212_473_000))
            .expect("reject stale result"),
        RecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed
    );
    assert!(
        catalog
            .library_place_resolution(312_304_000, 1_212_473_000)
            .expect("read old coordinates")
            .is_none()
    );
    let candidates = catalog
        .library_place_resolution_candidates(16)
        .expect("read current candidate");
    assert_eq!(candidates.len(), 1);
    assert_eq!(candidates[0].latitude_e7, 399_042_000);
}

#[test]
fn invalid_provider_results_never_enter_the_catalog() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    register_with_coordinates(
        &mut catalog,
        "/places/invalid.nef",
        312_304_000,
        1_212_473_000,
    );
    let mut record = shanghai_record(312_304_000, 1_212_473_000);
    record.country_code.clear();
    record.country_name.clear();
    assert!(catalog.record_library_place_resolution(&record).is_err());
}

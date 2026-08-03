use super::{asset_registration_fixture::register, library_fact_fixture::facts_for};
use crate::{
    Catalog, LibraryCoordinates, LibraryDateRange, LibraryMetadataOverrideAction,
    LibraryMetadataOverrideOrigin, LibraryPhotoFilter, SetPhotoLibraryMetadataOverrides,
};

fn command(
    photo_id: shadow_domain::PhotoId,
    capture_time: LibraryMetadataOverrideAction<i64>,
    coordinates: LibraryMetadataOverrideAction<LibraryCoordinates>,
    origin: LibraryMetadataOverrideOrigin,
    source_label: &str,
    updated_at_ms: i64,
) -> SetPhotoLibraryMetadataOverrides {
    SetPhotoLibraryMetadataOverrides {
        photo_id,
        capture_time,
        coordinates,
        origin,
        source_label: source_label.into(),
        updated_at_ms,
    }
}

#[test]
fn corrections_survive_rescan_and_drive_effective_library_queries() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = register(&mut catalog, "/archive/metadata-overrides.nef");
    let mut observed = facts_for(
        registered,
        Some(1_700_000_000),
        "Nikon Corporation",
        "Nikon Z 8",
    );
    observed.latitude_e7 = Some(399_000_000);
    observed.longitude_e7 = Some(1_164_000_000);
    catalog
        .upsert_photo_library_facts(&observed)
        .expect("record observed facts");

    // 2024-02-10 UTC, Chinese New Year.
    let corrected_time = 1_707_523_200;
    let corrected_coordinates = LibraryCoordinates {
        latitude_e7: 312_304_000,
        longitude_e7: 1_214_735_000,
        place_name: "Shanghai".into(),
    };
    catalog
        .set_photo_library_metadata_overrides(&command(
            registered.photo_id,
            LibraryMetadataOverrideAction::Set(corrected_time),
            LibraryMetadataOverrideAction::Set(corrected_coordinates.clone()),
            LibraryMetadataOverrideOrigin::Manual,
            "timezone and map correction",
            200,
        ))
        .expect("set corrections");

    let stored = catalog
        .photo_library_metadata_overrides(registered.photo_id)
        .expect("read corrections");
    assert_eq!(
        stored.capture_time.expect("capture override").value,
        Some(corrected_time)
    );
    assert_eq!(
        stored.coordinates.expect("coordinates override").value,
        Some(corrected_coordinates.clone())
    );
    assert_eq!(
        catalog
            .photo_library_facts(registered.photo_id)
            .expect("read observation")
            .expect("observed facts")
            .captured_at_unix_seconds,
        Some(1_700_000_000)
    );

    let mut rescanned = observed.clone();
    rescanned.captured_at_unix_seconds = Some(1_700_000_100);
    rescanned.camera_model = "Nikon Z 8 (rescanned)".into();
    rescanned.latitude_e7 = Some(400_000_000);
    rescanned.longitude_e7 = Some(1_165_000_000);
    rescanned.indexed_at_ms = 300;
    catalog
        .upsert_photo_library_facts(&rescanned)
        .expect("rescan observed facts");

    let effective = catalog
        .effective_photo_library_facts(registered.photo_id)
        .expect("read effective facts")
        .expect("effective facts");
    assert_eq!(
        effective.captured_at_unix_seconds,
        Some(corrected_time),
        "rescan must not replace the user correction"
    );
    assert_eq!(
        effective.latitude_e7,
        Some(corrected_coordinates.latitude_e7)
    );
    assert_eq!(effective.camera_model, "Nikon Z 8 (rescanned)");

    let matching = catalog
        .library_photo_count(&LibraryPhotoFilter {
            capture_time: Some(LibraryDateRange {
                start_inclusive: Some(corrected_time),
                end_inclusive: Some(corrected_time),
            }),
            ..LibraryPhotoFilter::default()
        })
        .expect("query corrected time");
    assert_eq!(matching, 1);
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                chinese_lunar_month: Some(1),
                chinese_lunar_day: Some(1),
                chinese_lunar_is_leap_month: Some(false),
                ..LibraryPhotoFilter::default()
            })
            .expect("query corrected Chinese lunar date"),
        1,
        "effective lunar indexes must follow the corrected capture day"
    );
}

#[test]
fn corrections_can_clear_or_return_to_decoder_observations() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = register(&mut catalog, "/archive/metadata-reset.nef");
    let mut observed = facts_for(registered, Some(1_700_000_000), "Pentax", "K10D");
    observed.latitude_e7 = Some(100_000_000);
    observed.longitude_e7 = Some(200_000_000);
    catalog
        .upsert_photo_library_facts(&observed)
        .expect("record observed facts");

    catalog
        .set_photo_library_metadata_overrides(&command(
            registered.photo_id,
            LibraryMetadataOverrideAction::Clear,
            LibraryMetadataOverrideAction::Clear,
            LibraryMetadataOverrideOrigin::Manual,
            "",
            200,
        ))
        .expect("clear effective metadata");
    let cleared = catalog
        .effective_photo_library_facts(registered.photo_id)
        .expect("read cleared facts")
        .expect("cleared effective facts");
    assert_eq!(cleared.captured_at_unix_seconds, None);
    assert_eq!(cleared.latitude_e7, None);
    assert_eq!(cleared.longitude_e7, None);

    catalog
        .set_photo_library_metadata_overrides(&command(
            registered.photo_id,
            LibraryMetadataOverrideAction::Inherit,
            LibraryMetadataOverrideAction::Inherit,
            LibraryMetadataOverrideOrigin::Manual,
            "",
            300,
        ))
        .expect("restore observations");
    let inherited = catalog
        .effective_photo_library_facts(registered.photo_id)
        .expect("read inherited facts")
        .expect("inherited effective facts");
    assert_eq!(inherited.captured_at_unix_seconds, Some(1_700_000_000));
    assert_eq!(inherited.latitude_e7, Some(100_000_000));
    assert_eq!(inherited.longitude_e7, Some(200_000_000));
    assert_eq!(
        catalog
            .photo_library_metadata_overrides(registered.photo_id)
            .expect("read reset override")
            .coordinates,
        None
    );
}

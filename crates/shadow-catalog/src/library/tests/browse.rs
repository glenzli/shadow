use super::{
    asset_registration_fixture::{register, register_kind},
    library_fact_fixture::facts_for,
};
use crate::{
    AlbumKind, Catalog, CommitRecipe, LibraryApertureRange, LibraryFacetKind, LibraryFacetValue,
    LibraryLivingPlaceRule, LibraryPhotoCursor, LibraryPhotoCursorValue, LibraryPhotoFilter,
    LibraryPhotoOrder, RecipeRefKind, RecipeRefTarget, RecordLibraryPlaceResolution,
    RecordLibraryPlaceResolutionStatus, SetPhotoLibraryState, library_equipment_key,
};
use shadow_domain::{
    EntityId, NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoFlag, RecipeCommit, RecipeCommitId,
    RecipeId, RecipeSnapshot, RepresentationKind,
};

#[test]
fn photo_first_library_page_includes_original_raster_sources() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let raw = register(&mut catalog, "/archive/original.nef");
    let raster = register_kind(
        &mut catalog,
        "/archive/original.jpg",
        RepresentationKind::OriginalRaster,
    );

    let page = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read Library page");
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count Library photos"),
        2
    );
    assert_eq!(page.items.len(), 2);
    assert!(page.items.iter().any(|item| {
        item.photo_id == raw.photo_id && item.location.display_path == "/archive/original.nef"
    }));
    assert!(page.items.iter().any(|item| {
        item.photo_id == raster.photo_id && item.location.display_path == "/archive/original.jpg"
    }));
}

#[test]
#[allow(clippy::too_many_lines)]
fn photo_first_library_page_filters_facets_and_keysets_without_path_ownership() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let newest = register(&mut catalog, "/one/source/first.nef");
    let middle = register(&mut catalog, "/other/source/second.nef");
    let unindexed = register(&mut catalog, "/third/source/third.nef");
    catalog
        .upsert_photo_library_facts(&facts_for(
            newest,
            Some(1_700_000_200),
            "Nikon Corporation",
            "Nikon Z 8",
        ))
        .expect("newest facts");
    catalog
        .upsert_photo_library_facts(&facts_for(middle, Some(1_700_000_100), "Pentax", "K10D"))
        .expect("middle facts");
    catalog
        .set_photo_library_state(&SetPhotoLibraryState {
            photo_id: newest.photo_id,
            liked: true,
            color_label: "blue".into(),
            updated_at_ms: 200,
        })
        .expect("like newest");
    catalog
        .append_photo_decision_event(&NewPhotoDecisionEvent {
            event_id: "library-filter-picked".into(),
            photo_id: newest.photo_id,
            occurred_at_unix_ms: 201,
            origin: PhotoDecisionOrigin::Human,
            expected_head_sequence: 0,
            before_flag: PhotoFlag::Unflagged,
            before_rating: 0,
            after_flag: PhotoFlag::Picked,
            after_rating: 5,
        })
        .expect("pick newest");
    let album = catalog
        .create_library_album(AlbumKind::Manual, "Portfolio", None, 202)
        .expect("album");
    catalog
        .add_photo_to_album(album.id, newest.photo_id, 0, 203)
        .expect("membership");
    let working_recipe = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Library filter fixture".into()),
        204,
    )
    .expect("create working Recipe");
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id: middle.photo_id,
            commit: working_recipe,
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: None,
            }],
        })
        .expect("persist working Recipe");

    let exact = catalog
        .library_photo_page(
            &LibraryPhotoFilter {
                camera_key: Some(library_equipment_key("Nikon Corporation", "Nikon Z 8")),
                lens_key: Some(library_equipment_key("Nikon", "NIKKOR Z 24-120mm f/4 S")),
                aperture: Some(LibraryApertureRange {
                    minimum_milli: Some(4_000),
                    maximum_milli: Some(4_000),
                }),
                liked: Some(true),
                color_label: Some("blue".into()),
                flag: Some(PhotoFlag::Picked),
                minimum_rating: Some(3),
                album_id: Some(album.id),
                ..LibraryPhotoFilter::default()
            },
            LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("all facets");
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                camera_key: Some(library_equipment_key("Nikon Corporation", "Nikon Z 8")),
                lens_key: Some(library_equipment_key("Nikon", "NIKKOR Z 24-120mm f/4 S")),
                aperture: Some(LibraryApertureRange {
                    minimum_milli: Some(4_000),
                    maximum_milli: Some(4_000),
                }),
                liked: Some(true),
                color_label: Some("blue".into()),
                flag: Some(PhotoFlag::Picked),
                minimum_rating: Some(3),
                album_id: Some(album.id),
                ..LibraryPhotoFilter::default()
            })
            .expect("exact count after facets settle"),
        1
    );
    assert_eq!(exact.items.len(), 1);
    assert_eq!(exact.items[0].photo_id, newest.photo_id);
    assert_eq!(
        exact.items[0].location.display_path,
        "/one/source/first.nef"
    );
    assert!(exact.items[0].state.liked);
    assert_eq!(exact.items[0].decision.flag, PhotoFlag::Picked);

    let edited = catalog
        .library_photo_page(
            &LibraryPhotoFilter {
                has_development_edits: Some(true),
                ..LibraryPhotoFilter::default()
            },
            LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read edited Library page");
    assert_eq!(edited.items.len(), 1);
    assert_eq!(edited.items[0].photo_id, middle.photo_id);
    assert!(edited.items[0].has_development_edits);
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                has_development_edits: Some(true),
                ..LibraryPhotoFilter::default()
            })
            .expect("count edited Library photos"),
        1
    );

    let unedited = catalog
        .library_photo_page(
            &LibraryPhotoFilter {
                has_development_edits: Some(false),
                ..LibraryPhotoFilter::default()
            },
            LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read unedited Library page");
    assert_eq!(unedited.items.len(), 2);
    assert!(
        unedited
            .items
            .iter()
            .all(|item| !item.has_development_edits)
    );

    let first_page = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::default(),
            None,
            2,
        )
        .expect("first page");
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count all photos"),
        3
    );
    assert_eq!(
        first_page
            .items
            .iter()
            .map(|item| item.photo_id)
            .collect::<Vec<_>>(),
        vec![newest.photo_id, middle.photo_id]
    );
    let second_page = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::default(),
            first_page.next_cursor.as_ref(),
            2,
        )
        .expect("second page");
    assert_eq!(
        second_page
            .items
            .iter()
            .map(|item| item.photo_id)
            .collect::<Vec<_>>(),
        vec![unindexed.photo_id]
    );
    assert!(second_page.next_cursor.is_none());
}

#[test]
fn library_order_is_keyset_stable_for_dates_and_file_names() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let zulu = register(&mut catalog, "/one/Zulu.NEF");
    let alpha = register(&mut catalog, "/two/alpha.nef");
    let middle = register(&mut catalog, "/three/Middle.nef");
    let _undated = register(&mut catalog, "/four/undated.nef");
    catalog
        .upsert_photo_library_facts(&facts_for(zulu, Some(300), "Nikon", "Z 8"))
        .expect("zulu facts");
    catalog
        .upsert_photo_library_facts(&facts_for(alpha, Some(100), "Nikon", "Z 8"))
        .expect("alpha facts");
    catalog
        .upsert_photo_library_facts(&facts_for(middle, Some(200), "Nikon", "Z 8"))
        .expect("middle facts");

    let paths = |page: &crate::LibraryPhotoPage| {
        page.items
            .iter()
            .map(|item| item.location.display_path.clone())
            .collect::<Vec<_>>()
    };
    let date_ascending = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::CaptureTimeAscending,
            None,
            16,
        )
        .expect("date ascending");
    assert_eq!(
        paths(&date_ascending),
        vec![
            "/two/alpha.nef",
            "/three/Middle.nef",
            "/one/Zulu.NEF",
            "/four/undated.nef",
        ]
    );

    let first_names = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::FileNameAscending,
            None,
            2,
        )
        .expect("first name page");
    assert_eq!(
        paths(&first_names),
        vec!["/two/alpha.nef", "/three/Middle.nef"]
    );
    let remaining_names = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::FileNameAscending,
            first_names.next_cursor.as_ref(),
            2,
        )
        .expect("remaining name page");
    assert_eq!(
        paths(&remaining_names),
        vec!["/four/undated.nef", "/one/Zulu.NEF"]
    );

    let names_descending = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::FileNameDescending,
            None,
            16,
        )
        .expect("name descending");
    assert_eq!(
        paths(&names_descending),
        vec![
            "/one/Zulu.NEF",
            "/four/undated.nef",
            "/three/Middle.nef",
            "/two/alpha.nef",
        ]
    );

    let mismatched_cursor = LibraryPhotoCursor {
        value: LibraryPhotoCursorValue::FileName("alpha.nef".into()),
        photo_id: alpha.photo_id,
    };
    assert!(
        catalog
            .library_photo_page(
                &LibraryPhotoFilter::default(),
                LibraryPhotoOrder::CaptureTimeDescending,
                Some(&mismatched_cursor),
                16,
            )
            .is_err()
    );
}

#[test]
fn bounded_library_facets_compose_without_directory_ownership() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let nikon_one = register(&mut catalog, "/roots/a/nikon-one.nef");
    let nikon_two = register(&mut catalog, "/roots/b/nikon-two.nef");
    let nikon_three = register(&mut catalog, "/roots/c/nikon-three.nef");
    let canon = register(&mut catalog, "/roots/d/canon.cr3");

    let mut facts = facts_for(
        nikon_one,
        Some(1_700_000_000),
        "Nikon Corporation",
        "Nikon Z 8",
    );
    facts.capture_day = "2024-03-18".into();
    catalog
        .upsert_photo_library_facts(&facts)
        .expect("first nikon facts");

    let mut facts = facts_for(
        nikon_two,
        Some(1_700_000_100),
        "Nikon Corporation",
        "Nikon Z 8",
    );
    facts.capture_day = "2024-03-03".into();
    catalog
        .upsert_photo_library_facts(&facts)
        .expect("second nikon facts");

    let mut facts = facts_for(
        nikon_three,
        Some(1_700_000_200),
        "Nikon Corporation",
        "Nikon Z 8",
    );
    facts.capture_day = "2024-02-16".into();
    facts.lens_model = "NIKKOR Z 50mm f/1.8 S".into();
    catalog
        .upsert_photo_library_facts(&facts)
        .expect("third nikon facts");

    let mut facts = facts_for(canon, Some(1_700_000_300), "Canon", "EOS R5");
    facts.capture_day = "2024-01-07".into();
    facts.lens_make = "Canon".into();
    facts.lens_model = "RF 24-105mm F4 L IS USM".into();
    catalog
        .upsert_photo_library_facts(&facts)
        .expect("canon facts");

    let cameras = catalog
        .library_facet_page(
            &LibraryPhotoFilter::default(),
            LibraryFacetKind::Camera,
            None,
            1,
        )
        .expect("first camera facet page");
    assert_eq!(cameras.items.len(), 1);
    assert_eq!(cameras.items[0].label, "Nikon Corporation Nikon Z 8");
    assert_eq!(cameras.items[0].photo_count, 3);
    let remaining_cameras = catalog
        .library_facet_page(
            &LibraryPhotoFilter::default(),
            LibraryFacetKind::Camera,
            cameras.next_cursor.as_ref(),
            1,
        )
        .expect("second camera facet page");
    assert_eq!(remaining_cameras.items.len(), 1);
    assert_eq!(remaining_cameras.items[0].label, "Canon EOS R5");

    let months = catalog
        .library_facet_page(
            &LibraryPhotoFilter::default(),
            LibraryFacetKind::CaptureMonth,
            None,
            16,
        )
        .expect("month facets");
    assert_eq!(
        months.items[0],
        LibraryFacetValue {
            key: "2024-03".into(),
            label: "2024-03".into(),
            photo_count: 2,
        }
    );
    assert_eq!(months.items.len(), 3);

    let march = LibraryPhotoFilter {
        capture_month: Some("2024-03".into()),
        ..LibraryPhotoFilter::default()
    };
    assert_eq!(catalog.library_photo_count(&march).expect("march count"), 2);
    let lenses = catalog
        .library_facet_page(&march, LibraryFacetKind::Lens, None, 16)
        .expect("month-constrained lens facets");
    assert_eq!(lenses.items.len(), 1);
    assert_eq!(lenses.items[0].photo_count, 2);
    assert_eq!(lenses.items[0].label, "Nikon NIKKOR Z 24-120mm f/4 S");

    assert!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                capture_month: Some("2024-13".into()),
                ..LibraryPhotoFilter::default()
            })
            .is_err()
    );
}

#[test]
fn chinese_lunar_filter_is_recurring_indexed_and_composes_with_gregorian_month() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let dates = [
        ("/lunar/new-year-2023.nef", "2023-01-22"),
        ("/lunar/regular-second-month.nef", "2023-02-20"),
        ("/lunar/leap-second-month.nef", "2023-03-22"),
        ("/lunar/new-year-2024.nef", "2024-02-10"),
    ];
    for (index, (path, day)) in dates.into_iter().enumerate() {
        let registered = register(&mut catalog, path);
        let mut facts = facts_for(
            registered,
            Some(1_700_000_000 + i64::try_from(index).expect("fixture index")),
            "Nikon",
            "Z 9",
        );
        facts.capture_day = day.into();
        catalog
            .upsert_photo_library_facts(&facts)
            .expect("persist lunar fixture");
    }

    let lunar_new_year = LibraryPhotoFilter {
        chinese_lunar_month: Some(1),
        chinese_lunar_day: Some(1),
        chinese_lunar_is_leap_month: Some(false),
        ..LibraryPhotoFilter::default()
    };
    assert_eq!(
        catalog
            .library_photo_count(&lunar_new_year)
            .expect("count recurring lunar new year"),
        2
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                capture_month: Some("2024-02".into()),
                ..lunar_new_year.clone()
            })
            .expect("compose lunar date with Gregorian month"),
        1
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                chinese_lunar_month: Some(2),
                chinese_lunar_day: Some(1),
                chinese_lunar_is_leap_month: Some(true),
                ..LibraryPhotoFilter::default()
            })
            .expect("count leap second-month day"),
        1
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter {
                chinese_lunar_month: Some(2),
                chinese_lunar_day: Some(1),
                chinese_lunar_is_leap_month: Some(false),
                ..LibraryPhotoFilter::default()
            })
            .expect("count regular second-month day"),
        1
    );
}

#[test]
fn chinese_lunar_filter_rejects_out_of_range_months_and_days() {
    let catalog = Catalog::open_in_memory().expect("open catalog");
    for filter in [
        LibraryPhotoFilter {
            chinese_lunar_month: Some(13),
            ..LibraryPhotoFilter::default()
        },
        LibraryPhotoFilter {
            chinese_lunar_day: Some(31),
            ..LibraryPhotoFilter::default()
        },
    ] {
        assert!(catalog.library_photo_count(&filter).is_err());
    }
}

fn resolved_place_catalog() -> Catalog {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let shanghai_one = register(&mut catalog, "/places/shanghai-one.nef");
    let shanghai_two = register(&mut catalog, "/places/shanghai-two.nef");
    let shanghai_unknown = register(&mut catalog, "/places/shanghai-unknown.nef");
    let tokyo = register(&mut catalog, "/places/tokyo.nef");

    for (index, registered) in [shanghai_one, shanghai_two].into_iter().enumerate() {
        let mut facts = facts_for(registered, Some(1_700_000_000), "Nikon", "Z 8");
        facts.capture_day = if index == 0 {
            "2019-04-12"
        } else {
            "2023-11-14"
        }
        .into();
        facts.latitude_e7 = Some(312_304_000);
        facts.longitude_e7 = Some(1_212_473_000);
        catalog
            .upsert_photo_library_facts(&facts)
            .expect("index Shanghai coordinates");
    }
    let mut unknown_facts = facts_for(shanghai_unknown, None, "Nikon", "Z 8");
    unknown_facts.latitude_e7 = Some(312_304_000);
    unknown_facts.longitude_e7 = Some(1_212_473_000);
    catalog
        .upsert_photo_library_facts(&unknown_facts)
        .expect("index undated Shanghai coordinates");
    let mut tokyo_facts = facts_for(tokyo, Some(1_700_000_100), "Canon", "EOS R5");
    tokyo_facts.latitude_e7 = Some(356_765_000);
    tokyo_facts.longitude_e7 = Some(1_397_650_000);
    catalog
        .upsert_photo_library_facts(&tokyo_facts)
        .expect("index Tokyo coordinates");

    for record in [
        RecordLibraryPlaceResolution {
            latitude_e7: 312_304_000,
            longitude_e7: 1_212_473_000,
            country_code: "cn".into(),
            country_name: "China".into(),
            administrative_area: "Shanghai".into(),
            locality: "Shanghai".into(),
            display_name: "Shanghai, China".into(),
            provider_id: "test".into(),
            provider_version: "1".into(),
            locale: "en".into(),
            resolved_at_ms: 200,
        },
        RecordLibraryPlaceResolution {
            latitude_e7: 356_765_000,
            longitude_e7: 1_397_650_000,
            country_code: "jp".into(),
            country_name: "Japan".into(),
            administrative_area: "Tokyo".into(),
            locality: "Tokyo".into(),
            display_name: "Tokyo, Japan".into(),
            provider_id: "test".into(),
            provider_version: "1".into(),
            locale: "en".into(),
            resolved_at_ms: 200,
        },
    ] {
        assert_eq!(
            catalog
                .record_library_place_resolution(&record)
                .expect("record place"),
            RecordLibraryPlaceResolutionStatus::Recorded
        );
    }
    catalog
}

#[test]
fn country_and_city_facets_compose_with_the_photo_query_contract() {
    let catalog = resolved_place_catalog();

    let countries = catalog
        .library_facet_page(
            &LibraryPhotoFilter::default(),
            LibraryFacetKind::Country,
            None,
            16,
        )
        .expect("country facets");
    assert_eq!(
        countries.items,
        vec![
            LibraryFacetValue {
                key: "cn".into(),
                label: "China".into(),
                photo_count: 3,
            },
            LibraryFacetValue {
                key: "jp".into(),
                label: "Japan".into(),
                photo_count: 1,
            },
        ]
    );

    let china = LibraryPhotoFilter {
        country_key: Some("CN".into()),
        ..LibraryPhotoFilter::default()
    };
    assert_eq!(catalog.library_photo_count(&china).expect("China count"), 3);
    let cities = catalog
        .library_facet_page(&china, LibraryFacetKind::City, None, 16)
        .expect("China city facets");
    assert_eq!(cities.items.len(), 1);
    assert_eq!(cities.items[0].label, "Shanghai · China");
    assert_eq!(cities.items[0].photo_count, 3);

    let shanghai = LibraryPhotoFilter {
        country_key: Some("cn".into()),
        locality_key: Some(cities.items[0].key.clone()),
        ..LibraryPhotoFilter::default()
    };
    let page = catalog
        .library_photo_page(&shanghai, LibraryPhotoOrder::default(), None, 16)
        .expect("Shanghai photo page");
    assert_eq!(page.items.len(), 3);
}

#[test]
fn living_place_periods_exclude_only_ordinary_life_from_travel() {
    let catalog = resolved_place_catalog();
    let shanghai_key = "cn\u{1f}shanghai\u{1f}shanghai";
    let travel = LibraryPhotoFilter {
        living_place_rules: vec![
            LibraryLivingPlaceRule {
                locality_key: shanghai_key.into(),
                start_month: Some("2020-01".into()),
                end_month: None,
            },
            LibraryLivingPlaceRule {
                locality_key: "jp\u{1f}tokyo\u{1f}tokyo".into(),
                start_month: None,
                end_month: None,
            },
        ],
        ..LibraryPhotoFilter::default()
    };
    assert_eq!(
        catalog
            .library_photo_count(&travel)
            .expect("time-aware Travel count"),
        1
    );
    let travel_countries = catalog
        .library_facet_page(&travel, LibraryFacetKind::Country, None, 16)
        .expect("travel country facets");
    assert_eq!(travel_countries.items.len(), 1);
    assert_eq!(travel_countries.items[0].key, "cn");
    assert_eq!(travel_countries.items[0].photo_count, 1);
}

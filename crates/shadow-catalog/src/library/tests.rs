use super::*;
use crate::{CommitRecipe, ImportSessionState, RecipeRefKind, RecipeRefTarget};
use shadow_domain::{
    EntityId, NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoFlag, Platform, RecipeCommit,
    RecipeCommitId, RecipeId, RecipeSnapshot, RepresentationKind,
};

fn register(catalog: &mut Catalog, path: &str) -> RegisteredAsset {
    register_kind(catalog, path, RepresentationKind::OriginalRaw)
}

fn register_kind(catalog: &mut Catalog, path: &str, kind: RepresentationKind) -> RegisteredAsset {
    catalog
        .register_asset(&RegisterAsset {
            kind,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register source")
}

fn register_scan_entry(
    catalog: &mut Catalog,
    session_id: ImportSessionId,
    path: &str,
    now_ms: i64,
) -> RegisteredAsset {
    let request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 100,
        modified_at_ms: Some(10),
        now_ms,
    };
    catalog
        .record_import_discovered(session_id, &request)
        .expect("record scan discovery");
    catalog
        .register_import_asset(session_id, &request)
        .expect("register scan entry")
}

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
        .library_photo_page(&LibraryPhotoFilter::default(), None, 16)
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
fn exact_content_identity_relinks_a_moved_file_without_changing_photo_identity() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = register(&mut catalog, "/archive/DSC_0001.NEF");
    let identity = ContentIdentity::whole_file_blake3([7; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 100,
                modified_at_ms: Some(10),
            },
            identity: identity.clone(),
            observed_at_ms: 30,
        })
        .expect("record identity");

    let moved = catalog
        .register_asset_with_content_identity(
            &RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/consolidated/2026/rename.nef".to_vec(),
                    "/consolidated/2026/rename.nef",
                ),
                byte_len: 101,
                modified_at_ms: Some(11),
                now_ms: 40,
            },
            &identity,
        )
        .expect("relink moved file");

    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_eq!(moved.status, crate::RegistrationStatus::NeedsRevalidation);
    assert_eq!(catalog.stats().expect("stats").photos, 1);
    assert_eq!(catalog.stats().expect("stats").locations, 2);
    assert_eq!(
        catalog.relink_match(&identity).expect("lookup identity"),
        Some(RelinkMatch {
            photo_id: original.photo_id,
            representation_id: original.representation_id,
        })
    );
}

#[test]
fn source_mutation_invalidates_identity_and_rejects_a_late_hash_result() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = register(&mut catalog, "/archive/rewritten.nef");
    let identity = ContentIdentity::whole_file_blake3([42; 32]);
    let original_source = RepresentationFingerprint {
        byte_len: 100,
        modified_at_ms: Some(10),
    };
    assert_eq!(
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: original_source,
                identity: identity.clone(),
                observed_at_ms: 30,
            })
            .expect("record original identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );

    let changed = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/archive/rewritten.nef".to_vec(),
                "/archive/rewritten.nef",
            ),
            byte_len: 101,
            modified_at_ms: Some(11),
            now_ms: 40,
        })
        .expect("observe rewritten source");
    assert_eq!(changed.status, crate::RegistrationStatus::NeedsRevalidation);
    assert_eq!(
        catalog
            .representation_fingerprint(original.representation_id)
            .expect("read current source"),
        RepresentationFingerprint {
            byte_len: 101,
            modified_at_ms: Some(11),
        }
    );
    assert_eq!(
        catalog
            .relink_match(&identity)
            .expect("lookup old identity"),
        None
    );

    assert_eq!(
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: original_source,
                identity: identity.clone(),
                observed_at_ms: 41,
            })
            .expect("late write is a normal stale result"),
        RecordRepresentationContentIdentityStatus::StaleSource
    );
    assert_eq!(
        catalog
            .relink_match(&identity)
            .expect("old identity stays absent"),
        None
    );
}

#[test]
fn likes_are_independent_from_stars_and_flags_and_survive_reopen() {
    let root = std::env::temp_dir().join(format!("shadow-library-like-{}", PhotoId::new_v7()));
    std::fs::create_dir_all(&root).expect("create root");
    let database = root.join("library.sqlite");
    let photo_id;
    {
        let mut catalog = Catalog::open(&database).expect("open catalog");
        photo_id = register(&mut catalog, "/archive/like.nef").photo_id;
        catalog
            .set_photo_library_state(&SetPhotoLibraryState {
                photo_id,
                liked: true,
                color_label: "blue".into(),
                updated_at_ms: 100,
            })
            .expect("like photo");
    }
    let catalog = Catalog::open(&database).expect("reopen catalog");
    let state = catalog.photo_library_state(photo_id).expect("read state");
    assert!(state.liked);
    assert_eq!(state.color_label, "blue");
    drop(catalog);
    std::fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn one_photo_can_belong_to_multiple_manual_albums() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo = register(&mut catalog, "/archive/albums.nef").photo_id;
    let first = catalog
        .create_library_album(AlbumKind::Manual, "Travel", None, 10)
        .expect("first album");
    let second = catalog
        .create_library_album(AlbumKind::Manual, "Favorites", None, 20)
        .expect("second album");
    catalog
        .add_photo_to_album(first.id, photo, 0, 30)
        .expect("add first membership");
    catalog
        .add_photo_to_album(second.id, photo, 0, 31)
        .expect("add second membership");
    // Repeated add is intentionally idempotent.
    catalog
        .add_photo_to_album(first.id, photo, 9, 32)
        .expect("idempotent membership");
    assert_eq!(catalog.albums_for_photo(photo).expect("albums").len(), 2);
    assert!(
        catalog
            .remove_photo_from_album(first.id, photo)
            .expect("remove membership")
    );
    let albums = catalog.albums_for_photo(photo).expect("remaining album");
    assert_eq!(albums, vec![second]);
}

#[test]
fn smart_album_query_v1_is_canonical_validated_and_uses_the_indexed_grid() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let matching = register(&mut catalog, "/archive/smart-match.nef");
    let excluded = register(&mut catalog, "/archive/smart-excluded.nef");
    catalog
        .upsert_photo_library_facts(&facts_for(
            matching,
            Some(1_700_000_200),
            "Nikon Corporation",
            "Nikon Z 8",
        ))
        .expect("index matching facts");
    catalog
        .upsert_photo_library_facts(&facts_for(excluded, Some(1_700_000_100), "Pentax", "K10D"))
        .expect("index excluded facts");
    catalog
        .set_photo_library_state(&SetPhotoLibraryState {
            photo_id: matching.photo_id,
            liked: true,
            color_label: "none".into(),
            updated_at_ms: 30,
        })
        .expect("like matching photo");

    let query = SmartAlbumQueryV1::new(LibraryPhotoFilter {
        camera_key: Some(library_equipment_key("Nikon Corporation", "Nikon Z 8")),
        liked: Some(true),
        ..LibraryPhotoFilter::default()
    })
    .expect("build smart query");
    let query_json = query.to_json().expect("serialize smart query");
    assert_eq!(query.schema_version(), SmartAlbumQueryV1::SCHEMA_VERSION);
    assert_eq!(
        SmartAlbumQueryV1::from_json(&query_json)
            .expect("read canonical query")
            .library_filter()
            .expect("get executable filter"),
        query.library_filter().expect("get original filter")
    );

    let album = catalog
        .create_smart_library_album("Nikon favorites", &query, 40)
        .expect("create smart album");
    assert_eq!(album.query_json.as_deref(), Some(query_json.as_str()));
    let page = catalog
        .smart_album_photo_page(album.id, None, 16)
        .expect("page smart album");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].photo_id, matching.photo_id);
    assert_eq!(
        catalog
            .smart_album_photo_count(album.id)
            .expect("count smart album"),
        1
    );

    assert!(
        catalog
            .create_library_album(AlbumKind::Smart, "Invalid", Some("{}"), 41)
            .is_err()
    );
    assert!(SmartAlbumQueryV1::from_json(r#"{"schema_version":2,"filter":{}}"#).is_err());
    assert!(
        SmartAlbumQueryV1::from_json(r#"{"schema_version":1,"filter":{},"typo":true}"#).is_err()
    );
    let membership_query = format!(
        r#"{{"schema_version":1,"filter":{{"album_id":"{}"}}}}"#,
        CollectionId::new_v7()
    );
    assert!(SmartAlbumQueryV1::from_json(&membership_query).is_err());
}

#[test]
fn album_management_renames_replaces_queries_and_deletes_only_the_album() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo = register(&mut catalog, "/archive/album-management.nef");
    let manual = catalog
        .create_library_album(AlbumKind::Manual, "Before", None, 10)
        .expect("create manual album");
    catalog
        .add_photo_to_album(manual.id, photo.photo_id, 0, 11)
        .expect("add manual membership");
    let renamed = catalog
        .rename_library_album(manual.id, "After", 12)
        .expect("rename manual album");
    assert_eq!(renamed.name, "After");
    assert_eq!(renamed.updated_at_ms, 12);

    let initial_query =
        SmartAlbumQueryV1::new(LibraryPhotoFilter::default()).expect("create initial smart query");
    let smart = catalog
        .create_smart_library_album("All photos", &initial_query, 20)
        .expect("create smart album");
    let refined_query = SmartAlbumQueryV1::new(LibraryPhotoFilter {
        liked: Some(true),
        ..LibraryPhotoFilter::default()
    })
    .expect("create refined smart query");
    let updated = catalog
        .replace_smart_album_query(smart.id, &refined_query, 21)
        .expect("replace smart query");
    assert_eq!(
        updated.query_json,
        Some(refined_query.to_json().expect("serialize query"))
    );
    assert_eq!(
        catalog
            .smart_album_filter(smart.id)
            .expect("read replaced smart query"),
        refined_query
            .library_filter()
            .expect("read expected filter")
    );
    assert!(
        catalog
            .replace_smart_album_query(manual.id, &initial_query, 22)
            .is_err()
    );

    assert!(
        catalog
            .delete_library_album(manual.id)
            .expect("delete manual album")
    );
    assert!(
        catalog
            .albums_for_photo(photo.photo_id)
            .expect("read memberships after deletion")
            .is_empty()
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("photo remains in Library"),
        1
    );
    assert!(
        !catalog
            .delete_library_album(manual.id)
            .expect("deleting absent album is idempotent")
    );
}

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
        longitude_e7: Some(116_400_0000),
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

fn facts_for(
    registered: RegisteredAsset,
    captured_at_unix_seconds: Option<i64>,
    make: &str,
    model: &str,
) -> LibraryPhotoFacts {
    LibraryPhotoFacts {
        photo_id: registered.photo_id,
        captured_at_unix_seconds,
        capture_day: captured_at_unix_seconds
            .map(|_| "2023-11-14".to_owned())
            .unwrap_or_default(),
        camera_make: make.into(),
        camera_model: model.into(),
        lens_make: "Nikon".into(),
        lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
        aperture_milli: Some(4_000),
        focal_length_tenth_mm: Some(240),
        iso_speed: Some(800.0),
        latitude_e7: None,
        longitude_e7: None,
        place_name: String::new(),
        indexed_representation_id: Some(registered.representation_id),
        indexed_source: Some(RepresentationFingerprint {
            byte_len: 100,
            modified_at_ms: Some(10),
        }),
        indexed_at_ms: 100,
    }
}

#[test]
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
        .library_photo_page(&LibraryPhotoFilter::default(), None, 2)
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
fn source_health_pages_scan_absences_without_marking_locations_offline() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let root = AssetLocation::new(Platform::MacOs, b"/archive".to_vec(), "/archive");
    let first_scan = catalog
        .begin_import_session(&root, 1)
        .expect("begin first source scan");
    let first = register_scan_entry(&mut catalog, first_scan, "/archive/first.nef", 2);
    let second = register_scan_entry(&mut catalog, first_scan, "/archive/second.nef", 3);
    let third = register_scan_entry(&mut catalog, first_scan, "/archive/third.nef", 4);
    catalog
        .finish_import_session(first_scan, ImportSessionState::Completed, None, 5)
        .expect("finish first source scan");

    let second_scan = catalog
        .begin_import_session(&root, 10)
        .expect("begin second source scan");
    let observed = register_scan_entry(&mut catalog, second_scan, "/archive/first.nef", 11);
    assert_eq!(observed.representation_id, first.representation_id);
    catalog
        .finish_import_session(second_scan, ImportSessionState::Completed, None, 12)
        .expect("finish second source scan");

    let health = catalog.library_source_health().expect("read source health");
    assert_eq!(health.len(), 1);
    let scan = health[0]
        .latest_completed_scan
        .as_ref()
        .expect("completed scan evidence");
    assert_eq!(scan.session_id, second_scan);
    assert_eq!(scan.known_locations, 3);
    assert_eq!(scan.seen_locations, 1);
    assert_eq!(scan.not_seen_locations, 2);

    let first_page = catalog
        .missing_source_location_page(second_scan, None, 1)
        .expect("page missing locations")
        .expect("durable source page");
    assert_eq!(first_page.reconciliation, *scan);
    assert_eq!(first_page.items.len(), 1);
    let relink_target = catalog
        .missing_source_relink_target(second_scan, first_page.items[0].location_id)
        .expect("read exact source relink target")
        .expect("first-page item remains valid reattach evidence");
    assert_eq!(relink_target.location, first_page.items[0]);
    assert!(
        catalog
            .missing_source_relink_target(second_scan, observed.location_id)
            .expect("check observed source")
            .is_none()
    );

    // Refresh the *other* missing location in a later scan. The review
    // must still page the old completed session without skipping it just
    // because mutable `last_seen_at_ms` changed in the meantime.
    let refreshed_representation =
        if first_page.items[0].representation_id == second.representation_id {
            third.representation_id
        } else {
            second.representation_id
        };
    let refreshed_path = if refreshed_representation == second.representation_id {
        "/archive/second.nef"
    } else {
        "/archive/third.nef"
    };
    let later_scan = catalog
        .begin_import_session(&root, 20)
        .expect("begin later source scan");
    let refreshed = register_scan_entry(&mut catalog, later_scan, refreshed_path, 21);
    assert_eq!(refreshed.representation_id, refreshed_representation);
    catalog
        .finish_import_session(later_scan, ImportSessionState::Completed, None, 22)
        .expect("finish later source scan");

    let second_page = catalog
        .missing_source_location_page(second_scan, first_page.next_cursor.as_ref(), 1)
        .expect("page remaining missing locations")
        .expect("durable source page");
    assert_eq!(second_page.items.len(), 1);
    assert!(second_page.next_cursor.is_none());

    let missing = first_page
        .items
        .iter()
        .chain(second_page.items.iter())
        .map(|item| item.representation_id)
        .collect::<Vec<_>>();
    assert_eq!(missing.len(), 2);
    assert!(missing.contains(&second.representation_id));
    assert!(missing.contains(&third.representation_id));
    assert_ne!(missing[0], missing[1]);
    assert!(
        first_page
            .items
            .iter()
            .chain(second_page.items.iter())
            .all(|item| item.location.display_path.starts_with("/archive/"))
    );

    // A source-specific absence does not make the source unavailable to
    // the normal photo-first Library query.
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count online Library originals"),
        3
    );
}

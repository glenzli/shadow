use super::{asset_registration_fixture::register, library_fact_fixture::facts_for};
use crate::{
    AlbumKind, Catalog, LibraryPhotoFilter, SetPhotoLibraryState, SmartAlbumQueryV1,
    library_equipment_key,
};
use shadow_domain::{CollectionId, EntityId, PhotoId};

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

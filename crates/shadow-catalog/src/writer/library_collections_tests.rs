use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    AlbumKind, LibraryPhotoFilter, RegisterAsset, SetPhotoLibraryState, SmartAlbumQueryV1,
};

use super::CatalogActor;

fn register_photo(actor: &CatalogActor, path: &str) -> shadow_domain::PhotoId {
    actor
        .handle()
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register Library photo")
        .photo_id
}

#[test]
fn actor_routes_photo_affinity_and_manual_album_membership() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let photo_id = register_photo(&actor, "/photos/manual-album.dng");

    let neutral = handle
        .photo_library_state(photo_id)
        .expect("read neutral state through actor");
    assert!(!neutral.liked);
    assert_eq!(neutral.color_label, "none");
    handle
        .set_photo_library_state(&SetPhotoLibraryState {
            photo_id,
            liked: true,
            color_label: "blue".into(),
            updated_at_ms: 1_700_000_000_010,
        })
        .expect("set affinity through actor");
    assert!(
        handle
            .photo_library_state(photo_id)
            .expect("read affinity through actor")
            .liked
    );

    let album = handle
        .create_library_album(AlbumKind::Manual, "Portfolio", None, 1_700_000_000_020)
        .expect("create manual album through actor");
    assert_eq!(
        handle.library_albums().expect("list albums through actor"),
        vec![album.clone()]
    );
    handle
        .add_photo_to_album(album.id, photo_id, 0, 1_700_000_000_021)
        .expect("add membership through actor");
    assert_eq!(
        handle
            .albums_for_photo(photo_id)
            .expect("list memberships through actor"),
        vec![album.clone()]
    );
    assert!(
        handle
            .remove_photo_from_album(album.id, photo_id)
            .expect("remove membership through actor")
    );
    assert!(
        handle
            .albums_for_photo(photo_id)
            .expect("read empty memberships through actor")
            .is_empty()
    );
    assert!(
        handle
            .delete_library_album(album.id)
            .expect("delete album through actor")
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_routes_smart_album_query_and_photo_page() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let photo_id = register_photo(&actor, "/photos/smart-album.dng");
    let query = SmartAlbumQueryV1::new(LibraryPhotoFilter::default())
        .expect("build all-photos smart query");
    let album = handle
        .create_smart_library_album("Everything", &query, 1_700_000_000_100)
        .expect("create smart album through actor");

    assert_eq!(
        handle
            .smart_album_filter(album.id)
            .expect("read smart filter through actor"),
        LibraryPhotoFilter::default()
    );
    let page = handle
        .smart_album_photo_page(album.id, None, 16)
        .expect("page smart album through actor");
    assert_eq!(page.items[0].photo_id, photo_id);
    assert_eq!(
        handle
            .smart_album_photo_count(album.id)
            .expect("count smart album through actor"),
        1
    );
    let renamed = handle
        .rename_library_album(album.id, "Everything renamed", 1_700_000_000_101)
        .expect("rename smart album through actor");
    assert_eq!(renamed.name, "Everything renamed");
    let refined_query = SmartAlbumQueryV1::new(LibraryPhotoFilter {
        liked: Some(true),
        ..LibraryPhotoFilter::default()
    })
    .expect("build refined smart query");
    let replaced = handle
        .replace_smart_album_query(album.id, &refined_query, 1_700_000_000_102)
        .expect("replace smart query through actor");
    assert_eq!(
        replaced.query_json,
        Some(refined_query.to_json().expect("serialize refined query"))
    );
    assert!(
        handle
            .delete_library_album(album.id)
            .expect("delete smart album through actor")
    );
    assert!(
        !handle
            .delete_library_album(album.id)
            .expect("idempotent deleted smart album through actor")
    );
    actor.shutdown().expect("shutdown actor");
}

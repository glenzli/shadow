use std::thread;

use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::RegisterAsset;

use super::*;

#[test]
fn cloned_handles_serialize_writes_through_one_actor() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handles = (0_u8..4)
        .map(|index| {
            let handle = actor.handle();
            thread::spawn(move || {
                let request = RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(
                        Platform::MacOs,
                        format!("/photos/{index}.nef").into_bytes(),
                        format!("/photos/{index}.nef"),
                    ),
                    byte_len: 42,
                    modified_at_ms: Some(100),
                    now_ms: 1_700_000_000_000,
                };
                handle.register_asset(&request).expect("register asset");
            })
        })
        .collect::<Vec<_>>();

    for handle in handles {
        handle.join().expect("join client thread");
    }
    assert_eq!(actor.handle().stats().expect("stats").photos, 4);
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_pages_photo_first_library_rows() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/library-page.dng".to_vec(),
                "/photos/library-page.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register library photo");
    let page = handle
        .library_photo_page(&LibraryPhotoFilter::default(), None, 16)
        .expect("page Library through actor");
    assert_eq!(
        handle
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count Library through actor"),
        1
    );
    assert_eq!(page.items[0].photo_id, registered.photo_id);
    assert_eq!(
        page.items[0].location.display_path,
        "/photos/library-page.dng"
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_creates_and_pages_a_v1_smart_album() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/smart-album.dng".to_vec(),
                "/photos/smart-album.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register Library photo");
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
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].photo_id, registered.photo_id);
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

#[test]
fn actor_resolves_an_original_raster_through_the_source_neutral_query() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/editable.jpg".to_vec(),
                "/photos/editable.jpg",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register raster photo");

    assert_eq!(
        handle
            .photo_source(registered.photo_id)
            .expect("resolve source-neutral photo source")
            .expect("online raster source")
            .representation_id,
        registered.representation_id
    );
    assert!(
        handle
            .review_source(registered.photo_id)
            .expect("resolve legacy RAW-only source")
            .is_none()
    );
    actor.shutdown().expect("shutdown actor");
}

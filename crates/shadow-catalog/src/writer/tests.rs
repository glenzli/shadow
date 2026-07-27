use std::thread;

use shadow_domain::{Platform, RepresentationKind};

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
fn actor_reads_exact_relink_matches_without_attaching_a_location() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/moved-source.nef".to_vec(),
                "/photos/moved-source.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3([7; 32]);
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: registered.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 2,
        })
        .expect("record exact identity");

    assert_eq!(
        handle.relink_match(&identity).expect("read actor match"),
        Some(RelinkMatch {
            photo_id: registered.photo_id,
            representation_id: registered.representation_id,
        })
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_rejects_a_late_content_identity_after_the_source_changes() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let original = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/replaced-in-place.nef".to_vec(),
                "/photos/replaced-in-place.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3([63; 32]);
    let old_record = RecordRepresentationContentIdentity {
        representation_id: original.representation_id,
        expected_source: RepresentationFingerprint {
            byte_len: 42,
            modified_at_ms: Some(100),
        },
        identity: identity.clone(),
        observed_at_ms: 2,
    };
    assert_eq!(
        handle
            .record_representation_content_identity(&old_record)
            .expect("record current identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );
    handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/replaced-in-place.nef".to_vec(),
                "/photos/replaced-in-place.nef",
            ),
            byte_len: 43,
            modified_at_ms: Some(101),
            now_ms: 3,
        })
        .expect("observe replacement");
    assert_eq!(
        handle
            .record_representation_content_identity(&old_record)
            .expect("late result is rejected"),
        RecordRepresentationContentIdentityStatus::StaleSource
    );
    assert_eq!(
        handle.relink_match(&identity).expect("lookup stale hash"),
        None
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_attaches_a_confirmed_relocation_only_through_the_journal() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let mut handle = actor.handle();
    let original = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/original.nef".to_vec(),
                "/photos/original.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([41; 32]);
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 2,
        })
        .expect("record identity");

    let session = handle
        .begin_import_session(
            &AssetLocation::new(Platform::MacOs, b"/consolidated".to_vec(), "/consolidated"),
            3,
        )
        .expect("begin import session");
    let moved_request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            Platform::MacOs,
            b"/consolidated/renamed.nef".to_vec(),
            "/consolidated/renamed.nef",
        ),
        byte_len: 42,
        modified_at_ms: Some(200),
        now_ms: 4,
    };
    handle
        .record_import_discovered(session, &moved_request)
        .expect("journal discovery");

    let moved = handle
        .register_import_verified_relocation(
            session,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect("attach through actor");
    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_eq!(handle.stats().expect("stats").locations, 2);
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
fn actor_persists_immutable_export_preset_revisions() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let first = handle
        .create_export_preset("Actor JPEG", r#"{"format":"jpeg","quality":80}"#, 1)
        .expect("create preset through actor");
    let second = handle
        .revise_export_preset(first.preset_id, r#"{"format":"jpeg","quality":90}"#, 2)
        .expect("revise preset through actor");

    assert_eq!(
        handle
            .export_preset_revisions(first.preset_id)
            .expect("read revisions through actor"),
        vec![second, first]
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

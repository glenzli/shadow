use shadow_catalog::{CatalogActor, LibraryPhotoFilter, RegisterAsset, SmartAlbumQueryV1};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};
use uuid::Uuid;

use super::{LibraryService, library_album_id_from_text, library_photo_id_from_text};

#[test]
fn album_service_keeps_manual_membership_and_smart_queries_separate() {
    let root =
        std::env::temp_dir().join(format!("shadow-library-service-album-{}", Uuid::now_v7()));
    std::fs::create_dir_all(&root).expect("create catalog fixture root");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let service = LibraryService::new(actor.handle());
    let registered = service
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/library-service-album.dng".to_vec(),
                "/photos/library-service-album.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register Library photo");
    let manual = service
        .create_manual_album("Travel", 2)
        .expect("create manual album");
    service
        .add_photo_to_manual_album(
            &manual.id.to_string(),
            &registered.photo_id.to_string(),
            0,
            3,
        )
        .expect("add manual membership");
    assert_eq!(
        service
            .albums_for_photo(&registered.photo_id.to_string())
            .expect("read manual membership"),
        vec![manual.clone()]
    );
    let renamed = service
        .rename_album(&manual.id.to_string(), "Travel 2026", 4)
        .expect("rename manual album");
    assert_eq!(renamed.name, "Travel 2026");

    let query = SmartAlbumQueryV1::new(LibraryPhotoFilter::default())
        .expect("create all-photos smart query");
    let smart = service
        .create_smart_album("All photos", &query, 5)
        .expect("create smart album");
    assert_eq!(
        service
            .smart_album_photo_count(&smart.id.to_string())
            .expect("count smart album"),
        1
    );
    let replaced = service
        .replace_smart_album_query(&smart.id.to_string(), &query, 6)
        .expect("replace smart album query");
    assert_eq!(replaced.updated_at_ms, 6);
    assert!(
        service
            .remove_photo_from_manual_album(
                &manual.id.to_string(),
                &registered.photo_id.to_string(),
            )
            .expect("remove manual membership")
    );
    assert!(
        service
            .delete_album(&manual.id.to_string())
            .expect("delete manual album")
    );
    assert_eq!(
        service.albums().expect("list remaining albums"),
        vec![replaced]
    );
    assert!(library_album_id_from_text("").is_err());
    assert!(library_photo_id_from_text("not-a-photo-id").is_err());
    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove catalog fixture root");
}

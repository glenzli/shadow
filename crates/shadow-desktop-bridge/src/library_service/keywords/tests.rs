use shadow_catalog::{CatalogActor, RegisterAsset};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};
use uuid::Uuid;

use super::{LibraryService, keyword_id_from_text};

#[test]
fn service_routes_taxonomy_and_manual_batch_assignment_without_ffi_policy() {
    let root =
        std::env::temp_dir().join(format!("shadow-library-service-keyword-{}", Uuid::now_v7()));
    std::fs::create_dir_all(&root).expect("create catalog fixture root");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let service = LibraryService::new(actor.handle());
    let registered = service
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/library-service-keyword.dng".to_vec(),
                "/photos/library-service-keyword.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register Library photo");

    let root_keyword = service
        .create_keyword("", "Location", 2)
        .expect("create root keyword");
    let child = service
        .create_keyword(&root_keyword.id.to_string(), "Coast", 3)
        .expect("create child keyword");
    assert_eq!(service.keyword_tree().expect("read keyword tree").len(), 2);
    let child = service
        .rename_keyword(&child.id.to_string(), "Coastline", 3)
        .expect("rename child keyword");
    let receipt = service
        .assign_manual_keyword(&child.id.to_string(), &[registered.photo_id.to_string()], 4)
        .expect("assign keyword");
    assert_eq!(receipt.requested_photo_count, 1);
    assert_eq!(receipt.changed_photo_count, 1);
    assert_eq!(
        service
            .keywords_for_photo(&registered.photo_id.to_string())
            .expect("read photo keywords")[0]
            .keyword
            .id,
        child.id
    );

    let moved = service
        .move_keyword(&child.id.to_string(), "", 5)
        .expect("move child to root");
    assert_eq!(moved.parent_id, None);
    assert_eq!(
        service
            .remove_keyword(&child.id.to_string(), &[registered.photo_id.to_string()],)
            .expect("remove keyword")
            .changed_photo_count,
        1
    );
    assert_eq!(
        service
            .delete_keyword_subtree(&root_keyword.id.to_string())
            .expect("delete old root")
            .deleted_keyword_count,
        1
    );
    assert!(keyword_id_from_text("").is_err());
    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove catalog fixture root");
}

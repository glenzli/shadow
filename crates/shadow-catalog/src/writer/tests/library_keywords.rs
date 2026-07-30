use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{LibraryKeywordAssignmentOrigin, LibraryPhotoFilter, RegisterAsset};

use crate::writer::CatalogActor;

#[test]
fn actor_routes_keyword_taxonomy_assignment_and_filtering() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let photo = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/actor-keyword.dng".to_vec(),
                "/photos/actor-keyword.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register photo")
        .photo_id;

    let root = handle
        .create_library_keyword(None, "Nature", 1_700_000_000_010)
        .expect("create keyword through actor");
    let child = handle
        .create_library_keyword(Some(root.id), "Forest", 1_700_000_000_011)
        .expect("create child through actor");
    let receipt = handle
        .assign_library_keyword_to_photos(
            child.id,
            &[photo],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            1_700_000_000_020,
        )
        .expect("assign keyword through actor");
    assert_eq!(receipt.changed_photo_count, 1);
    assert_eq!(
        handle
            .library_keywords_for_photo(photo)
            .expect("read assignment through actor")[0]
            .keyword
            .id,
        child.id
    );
    assert_eq!(
        handle
            .library_photo_count(&LibraryPhotoFilter {
                keyword_ids_all: vec![root.id],
                ..LibraryPhotoFilter::default()
            })
            .expect("filter through actor"),
        1
    );
    assert_eq!(
        handle
            .remove_library_keyword_from_photos(child.id, &[photo])
            .expect("remove assignment through actor")
            .changed_photo_count,
        1
    );
    assert_eq!(
        handle
            .delete_library_keyword_subtree(root.id)
            .expect("delete subtree through actor")
            .deleted_keyword_count,
        2
    );
    actor.shutdown().expect("shutdown actor");
}

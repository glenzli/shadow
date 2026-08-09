use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    CreatePhotoGroup, PhotoGroupKind, PhotoGroupOrigin, RegisterAsset, writer::CatalogActor,
};

fn register_photo(actor: &CatalogActor, filename: &str) -> shadow_domain::PhotoId {
    let path = format!("/photos/{filename}");
    actor
        .handle()
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register relationship photo")
        .photo_id
}

#[test]
fn actor_routes_the_complete_distinct_photo_group_lifecycle() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let first = register_photo(&actor, "A.NEF");
    let second = register_photo(&actor, "B.NEF");
    let third = register_photo(&actor, "C.NEF");

    let created = handle
        .create_photo_group(&CreatePhotoGroup {
            kind: PhotoGroupKind::Panorama,
            origin: PhotoGroupOrigin::User,
            ordered_photo_ids: vec![first, second],
            anchor_photo_id: first,
            now_ms: 100,
        })
        .expect("create through actor");
    assert_eq!(
        handle.photo_group(created.id).expect("read through actor"),
        created
    );

    let replaced = handle
        .replace_photo_group_members(created.id, &[second, third], third, 200)
        .expect("replace through actor");
    assert_eq!(replaced.members[1].photo_id, third);
    assert!(replaced.members[1].is_anchor);
    assert_eq!(
        handle
            .photo_groups_for_photo(second)
            .expect("list through actor"),
        vec![replaced]
    );
    assert!(
        handle
            .delete_photo_group(created.id)
            .expect("delete through actor")
    );
    actor.shutdown().expect("shutdown actor");
}

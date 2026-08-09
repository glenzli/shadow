use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::*;
use crate::{RegisterAsset, RegistrationStatus};

fn register(catalog: &mut Catalog, filename: &str) -> PhotoId {
    let path = format!("/photos/{filename}");
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register photo");
    assert_eq!(registered.status, RegistrationStatus::Inserted);
    registered.photo_id
}

#[test]
fn persists_ordered_distinct_photos_without_merging_identity() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let first = register(&mut catalog, "A.NEF");
    let second = register(&mut catalog, "B.NEF");
    let third = register(&mut catalog, "C.NEF");

    let group = catalog
        .create_photo_group(&CreatePhotoGroup {
            kind: PhotoGroupKind::Burst,
            origin: PhotoGroupOrigin::CameraMetadata,
            ordered_photo_ids: vec![first, second, third],
            anchor_photo_id: second,
            now_ms: 100,
        })
        .expect("create burst");

    assert_eq!(catalog.stats().expect("stats").photos, 3);
    assert_eq!(group.kind, PhotoGroupKind::Burst);
    assert_eq!(group.members[0].photo_id, first);
    assert!(group.members[1].is_anchor);
    assert_eq!(group.members[2].position, 2);
    assert_eq!(
        catalog
            .photo_groups_for_photo(third)
            .expect("groups for third"),
        vec![group]
    );
}

#[test]
fn replacement_is_atomic_and_group_deletion_preserves_photos() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let first = register(&mut catalog, "A.NEF");
    let second = register(&mut catalog, "B.NEF");
    let third = register(&mut catalog, "C.NEF");
    let group = catalog
        .create_photo_group(&CreatePhotoGroup {
            kind: PhotoGroupKind::Similar,
            origin: PhotoGroupOrigin::User,
            ordered_photo_ids: vec![first, second],
            anchor_photo_id: first,
            now_ms: 100,
        })
        .expect("create similarity group");

    let replaced = catalog
        .replace_photo_group_members(group.id, &[third, second], second, 200)
        .expect("replace members");
    assert_eq!(replaced.created_at_ms, 100);
    assert_eq!(replaced.updated_at_ms, 200);
    assert_eq!(replaced.members[0].photo_id, third);
    assert!(replaced.members[1].is_anchor);
    assert!(
        catalog
            .photo_groups_for_photo(first)
            .expect("removed memberships")
            .is_empty()
    );

    assert!(catalog.delete_photo_group(group.id).expect("delete group"));
    assert_eq!(catalog.stats().expect("stats").photos, 3);
}

#[test]
fn rejects_duplicate_singleton_and_foreign_anchor_groups() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let first = register(&mut catalog, "A.NEF");
    let second = register(&mut catalog, "B.NEF");

    for request in [
        CreatePhotoGroup {
            kind: PhotoGroupKind::Bracket,
            origin: PhotoGroupOrigin::User,
            ordered_photo_ids: vec![first],
            anchor_photo_id: first,
            now_ms: 1,
        },
        CreatePhotoGroup {
            kind: PhotoGroupKind::Bracket,
            origin: PhotoGroupOrigin::User,
            ordered_photo_ids: vec![first, first],
            anchor_photo_id: first,
            now_ms: 1,
        },
        CreatePhotoGroup {
            kind: PhotoGroupKind::Bracket,
            origin: PhotoGroupOrigin::User,
            ordered_photo_ids: vec![first, second],
            anchor_photo_id: PhotoId::new_v7(),
            now_ms: 1,
        },
    ] {
        assert!(matches!(
            catalog.create_photo_group(&request),
            Err(CatalogError::InvalidPhotoGroup(_))
        ));
    }
}

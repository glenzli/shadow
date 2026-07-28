use shadow_domain::{
    EditEntityEntryV1, EditEntityMapV1, EditObjectId, EditObjectKind, LibraryRootV1,
};

use crate::{Catalog, CatalogError, CommitEditRepository, EditObjectPackWrite};

use super::object_graph_fixtures::{commit, initial_pack, leaf};

#[test]
fn object_pack_is_atomic_order_independent_and_idempotent() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let (pack, root_id) = initial_pack();
    let stored = catalog.store_edit_object_pack(&pack).unwrap();
    assert_eq!(stored.inserted, 3);
    assert_eq!(stored.reused, 0);
    let repeated = catalog.store_edit_object_pack(&pack).unwrap();
    assert_eq!(repeated.inserted, 0);
    assert_eq!(repeated.reused, 3);
    let root = catalog.edit_object(root_id).unwrap().unwrap();
    assert_eq!(root.object.kind(), EditObjectKind::LibraryRoot);
    assert_eq!(root.edges.len(), 1);
}

#[test]
fn missing_edge_target_rolls_back_the_entire_pack() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let missing = EditObjectId::from_bytes([9; 32]);
    let map = EditEntityMapV1::new(vec![EditEntityEntryV1 {
        key: "photo/missing".into(),
        value: missing,
    }])
    .unwrap()
    .into_object_pack()
    .unwrap();
    let map_id = map.object().id();
    let error = catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![map],
            created_at_ms: 1,
        })
        .unwrap_err();
    assert!(matches!(error, CatalogError::EditObjectNotFound(id) if id == missing));
    assert!(catalog.edit_object(map_id).unwrap().is_none());
}

#[test]
fn library_roots_and_commit_roots_fail_closed_on_wrong_object_kinds() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let photo = leaf(EditObjectKind::PhotoEditState, "not-a-map");
    let forged_root_payload = LibraryRootV1 {
        photo_recipes: Some(photo.object().id()),
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    };
    let forged_root = forged_root_payload.into_object_pack().unwrap();
    let forged_root_id = forged_root.object().id();
    let error = catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![forged_root, photo.clone()],
            created_at_ms: 1,
        })
        .unwrap_err();
    assert!(matches!(error, CatalogError::InvalidEditObject(_)));
    assert!(catalog.edit_object(forged_root_id).unwrap().is_none());

    catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![photo.clone()],
            created_at_ms: 2,
        })
        .unwrap();
    let invalid_commit = commit(photo.object().id(), Vec::new(), 3);
    let error = catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: invalid_commit.clone(),
            update_refs: Vec::new(),
        })
        .unwrap_err();
    assert!(matches!(
        error,
        CatalogError::InvalidEditRepositoryCommit(_)
    ));
    assert!(
        catalog
            .edit_repository_commit(invalid_commit.id())
            .unwrap()
            .is_none()
    );
}

use shadow_domain::{
    EditEntityEntryV1, EditEntityMapV1, EditObjectId, EditObjectKind, EditRepositoryRefExpectation,
    EditRepositoryRefKind, EntityId, LayerId, LayerRevisionId, LibraryRootV1,
};

use crate::{
    Catalog, CatalogError, CommitEditRepository, EditObjectPackWrite, EditRepositoryRefUpdate,
};

use super::object_graph_fixtures::{commit, initial_pack, leaf, shared_grade_revision};

#[test]
fn global_commit_and_ref_cas_are_atomic() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let (pack, root_id) = initial_pack();
    catalog.store_edit_object_pack(&pack).unwrap();
    let root_commit = commit(root_id, Vec::new(), 10);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: root_commit.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 10,
            }],
        })
        .unwrap();
    assert_eq!(
        catalog
            .edit_repository_ref("heads/main")
            .unwrap()
            .unwrap()
            .commit_id,
        root_commit.id()
    );

    let next = commit(root_id, vec![root_commit.id()], 20);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: next.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::At(root_commit.id()),
                updated_at_ms: 20,
            }],
        })
        .unwrap();
    assert_eq!(
        catalog
            .edit_repository_commit(next.id())
            .unwrap()
            .unwrap()
            .commit,
        next
    );

    let stale = commit(root_id, vec![root_commit.id()], 30);
    let error = catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: stale.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::At(root_commit.id()),
                updated_at_ms: 30,
            }],
        })
        .unwrap_err();
    assert!(matches!(
        error,
        CatalogError::EditRepositoryRefExpectationMismatch { .. }
    ));
    assert!(
        catalog
            .edit_repository_commit(stale.id())
            .unwrap()
            .is_none()
    );
    assert_eq!(
        catalog
            .edit_repository_ref("heads/main")
            .unwrap()
            .unwrap()
            .commit_id,
        next.id()
    );
}

#[test]
#[allow(clippy::similar_names, clippy::too_many_lines)]
fn one_library_commit_captures_photo_and_shared_grade_changes_together() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let photo_a_v1 = leaf(EditObjectKind::PhotoEditState, "photo-a-v1");
    let photo_b_v1 = leaf(EditObjectKind::PhotoEditState, "photo-b-v1");
    let shared_layer_id = LayerId::new_v7();
    let shared_v1_id = LayerRevisionId::new_v7();
    let shared_v1 = shared_grade_revision(shared_v1_id, shared_layer_id, 1, None, "shared-warm-v1");
    let photos_v1 = EditEntityMapV1::new(vec![
        EditEntityEntryV1 {
            key: "photo/a".into(),
            value: photo_a_v1.object().id(),
        },
        EditEntityEntryV1 {
            key: "photo/b".into(),
            value: photo_b_v1.object().id(),
        },
    ])
    .unwrap();
    let shared_heads_v1 = EditEntityMapV1::new(vec![EditEntityEntryV1 {
        key: "grade/warm".into(),
        value: shared_v1.object().id(),
    }])
    .unwrap();
    let photos_v1_pack = photos_v1.clone().into_object_pack().unwrap();
    let shared_v1_pack = shared_heads_v1.clone().into_object_pack().unwrap();
    let root_v1 = LibraryRootV1 {
        photo_recipes: Some(photos_v1_pack.object().id()),
        shared_grade_heads: Some(shared_v1_pack.object().id()),
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .unwrap();
    let root_v1_id = root_v1.object().id();
    catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![
                root_v1,
                photos_v1_pack,
                shared_v1_pack,
                photo_a_v1,
                photo_b_v1.clone(),
                shared_v1,
            ],
            created_at_ms: 10,
        })
        .unwrap();
    let commit_v1 = commit(root_v1_id, Vec::new(), 11);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: commit_v1.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 11,
            }],
        })
        .unwrap();

    let photo_a_v2 = leaf(EditObjectKind::PhotoEditState, "photo-a-v2");
    let shared_v2 = shared_grade_revision(
        LayerRevisionId::new_v7(),
        shared_layer_id,
        2,
        Some(shared_v1_id),
        "shared-warm-v2",
    );
    let photos_v2 = photos_v1
        .with_entry("photo/a", photo_a_v2.object().id())
        .unwrap();
    let shared_heads_v2 = shared_heads_v1
        .with_entry("grade/warm", shared_v2.object().id())
        .unwrap();
    let photos_v2_pack = photos_v2.clone().into_object_pack().unwrap();
    let shared_v2_pack = shared_heads_v2.clone().into_object_pack().unwrap();
    let root_v2 = LibraryRootV1 {
        photo_recipes: Some(photos_v2_pack.object().id()),
        shared_grade_heads: Some(shared_v2_pack.object().id()),
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .unwrap();
    let root_v2_id = root_v2.object().id();
    catalog
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![
                root_v2,
                photos_v2_pack,
                shared_v2_pack,
                photo_a_v2,
                photo_b_v1,
                shared_v2,
            ],
            created_at_ms: 20,
        })
        .unwrap();
    let commit_v2 = commit(root_v2_id, vec![commit_v1.id()], 21);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: commit_v2.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::At(commit_v1.id()),
                updated_at_ms: 21,
            }],
        })
        .unwrap();

    assert_eq!(photos_v1.diff(&photos_v2).len(), 1);
    assert_eq!(shared_heads_v1.diff(&shared_heads_v2).len(), 1);
    assert_eq!(commit_v2.payload().root, root_v2_id);
    assert_eq!(commit_v2.payload().parents, vec![commit_v1.id()]);
    assert_eq!(
        catalog
            .edit_repository_ref("heads/main")
            .unwrap()
            .unwrap()
            .commit_id,
        commit_v2.id()
    );
}

#[test]
fn immutable_objects_and_commits_survive_reopen() {
    let root = std::env::temp_dir().join(format!(
        "shadow-edit-repository-{}-{}",
        std::process::id(),
        EditObjectId::from_bytes([7; 32])
    ));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("catalog.sqlite");
    let (pack, root_id) = initial_pack();
    let root_commit = commit(root_id, Vec::new(), 10);
    {
        let mut catalog = Catalog::open(&path).unwrap();
        catalog.store_edit_object_pack(&pack).unwrap();
        catalog
            .commit_edit_repository(&CommitEditRepository {
                commit: root_commit.clone(),
                update_refs: Vec::new(),
            })
            .unwrap();
    }
    let catalog = Catalog::open(&path).unwrap();
    assert_eq!(
        catalog.edit_object(root_id).unwrap().unwrap().object.id(),
        root_id
    );
    assert_eq!(
        catalog
            .edit_repository_commit(root_commit.id())
            .unwrap()
            .unwrap()
            .commit,
        root_commit
    );
    drop(catalog);
    std::fs::remove_dir_all(root).unwrap();
}

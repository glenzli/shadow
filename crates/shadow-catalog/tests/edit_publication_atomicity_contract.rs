use shadow_catalog::{
    Catalog, CatalogError, CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository,
    EditObjectPackWrite, EditRepositoryRefUpdate, RecipeRefExpectation, RecipeRefKind,
    RecipeRefTarget, RegisterAsset,
};
use shadow_domain::{
    AssetLocation, EditCommitId, EditEntityEntryV1, EditEntityMapV1, EditObject, EditObjectId,
    EditObjectKind, EditObjectPack, EditRepositoryCommit, EditRepositoryCommitPayloadV1,
    EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId, LibraryRootV1, Platform,
    RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind,
};

fn initial_pack() -> (EditObjectPackWrite, EditObjectId) {
    let object = EditObject::from_canonical_json(
        EditObjectKind::PhotoEditState,
        1,
        &serde_json::json!({ "label": "photo-a" }),
    )
    .unwrap();
    let photo = EditObjectPack::new(object, Vec::new()).unwrap();
    let map = EditEntityMapV1::new(vec![EditEntityEntryV1 {
        key: "photo/00000000-0000-7000-8000-000000000001".into(),
        value: photo.object().id(),
    }])
    .unwrap()
    .into_object_pack()
    .unwrap();
    let root = LibraryRootV1 {
        photo_recipes: Some(map.object().id()),
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .unwrap();
    let root_id = root.object().id();
    (
        EditObjectPackWrite {
            objects: vec![root, map, photo],
            created_at_ms: 1_721_500_000_000,
        },
        root_id,
    )
}

fn commit(root: EditObjectId, parents: Vec<EditCommitId>, at: i64) -> EditRepositoryCommit {
    EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root,
        parents,
        message: Some(format!("Library checkpoint {at}")),
        created_at_ms: at,
    })
    .unwrap()
}

#[test]
#[allow(clippy::too_many_lines)]
fn stale_library_head_rolls_back_legacy_and_library_commits_together() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let photo_id = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/atomic-library.dng".to_vec(),
                "/photos/atomic-library.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(1),
            now_ms: 1,
        })
        .unwrap()
        .photo_id;
    let (pack, root_id) = initial_pack();
    catalog.store_edit_object_pack(&pack).unwrap();
    let library_root = commit(root_id, Vec::new(), 10);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: library_root.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 10,
            }],
        })
        .unwrap();
    let recipe_root = RecipeCommit::new(
        shadow_domain::RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Recipe root".into()),
        10,
    )
    .unwrap();
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: recipe_root.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .unwrap();

    let recipe_child = RecipeCommit::new(
        shadow_domain::RecipeCommitId::new_v7(),
        recipe_root.recipe_id(),
        vec![recipe_root.id()],
        RecipeSnapshot::empty(),
        Some("Must roll back".into()),
        20,
    )
    .unwrap();
    let stale_library_commit = commit(root_id, vec![library_root.id()], 20);
    let error = catalog
        .commit_recipe_and_edit_repository(&CommitRecipeAndEditRepository {
            recipe: CommitRecipe {
                photo_id,
                commit: recipe_child.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::At(recipe_root.id())),
                }],
            },
            repository: CommitEditRepository {
                commit: stale_library_commit.clone(),
                update_refs: vec![EditRepositoryRefUpdate {
                    name: "heads/main".into(),
                    kind: EditRepositoryRefKind::Branch,
                    expected: EditRepositoryRefExpectation::Missing,
                    updated_at_ms: 20,
                }],
            },
        })
        .unwrap_err();
    assert!(matches!(
        error,
        CatalogError::EditRepositoryRefExpectationMismatch { .. }
    ));
    assert!(
        catalog
            .recipe_commit(photo_id, recipe_child.id())
            .unwrap()
            .is_none()
    );
    assert!(
        catalog
            .edit_repository_commit(stale_library_commit.id())
            .unwrap()
            .is_none()
    );
    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .unwrap()
            .unwrap()
            .commit_id,
        recipe_root.id()
    );
    assert_eq!(
        catalog
            .edit_repository_ref("heads/main")
            .unwrap()
            .unwrap()
            .commit_id,
        library_root.id()
    );
}

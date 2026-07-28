use shadow_domain::{
    AssetLocation, EditEntityEntryV1, EditEntityMapV1, EditObject, EditObjectKind, EditObjectPack,
    EditRepositoryCommit, EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation,
    EditRepositoryRefKind, EntityId, LibraryRootV1, Platform, RecipeCommit, RecipeCommitId,
    RecipeId, RecipeSnapshot, RepresentationKind,
};

use crate::{
    CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository, EditObjectPackWrite,
    EditRepositoryRefUpdate, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget, RegisterAsset,
};

use crate::writer::CatalogActor;

#[test]
fn actor_resolves_exact_recipe_commits_without_crossing_photo_owners() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let register = |path: &str| RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 42,
        modified_at_ms: Some(100),
        now_ms: 1_700_000_000_000,
    };
    let owner = handle
        .register_asset(&register("/photos/exact-owner.dng"))
        .expect("register commit owner");
    let other = handle
        .register_asset(&register("/photos/exact-other.dng"))
        .expect("register other photo");
    let commit = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Exact actor lookup".to_owned()),
        1_700_000_001_000,
    )
    .expect("build Recipe commit");
    let expected = handle
        .commit_recipe(&CommitRecipe {
            photo_id: owner.photo_id,
            commit: commit.clone(),
            update_refs: Vec::new(),
        })
        .expect("commit Recipe through actor");

    assert_eq!(
        handle
            .recipe_commit(owner.photo_id, commit.id())
            .expect("resolve exact commit through actor"),
        Some(expected)
    );
    assert!(
        handle
            .recipe_commit(other.photo_id, commit.id())
            .expect("query commit through the wrong owner")
            .is_none()
    );
    assert!(
        handle
            .recipe_commit(owner.photo_id, RecipeCommitId::new_v7())
            .expect("query absent exact commit through actor")
            .is_none()
    );

    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_serializes_library_object_pack_commit_and_ref() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let photo = EditObject::from_canonical_json(
        EditObjectKind::PhotoEditState,
        1,
        &serde_json::json!({ "photo": "actor-photo" }),
    )
    .expect("build photo object");
    let photo = EditObjectPack::new(photo, Vec::new()).expect("pack photo object");
    let photo_map = EditEntityMapV1::new(vec![EditEntityEntryV1 {
        key: "photo/actor-photo".into(),
        value: photo.object().id(),
    }])
    .expect("build photo map")
    .into_object_pack()
    .expect("pack photo map");
    let root = LibraryRootV1 {
        photo_recipes: Some(photo_map.object().id()),
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .expect("pack Library root");
    let root_id = root.object().id();
    assert_eq!(
        handle
            .store_edit_object_pack(&EditObjectPackWrite {
                objects: vec![root, photo_map, photo],
                created_at_ms: 10,
            })
            .expect("store object pack")
            .inserted,
        3
    );
    let commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root: root_id,
        parents: Vec::new(),
        message: Some("Actor Library checkpoint".into()),
        created_at_ms: 11,
    })
    .expect("build Library commit");
    handle
        .commit_edit_repository(&CommitEditRepository {
            commit: commit.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 11,
            }],
        })
        .expect("commit Library state");

    assert_eq!(
        handle
            .edit_repository_commit(commit.id())
            .expect("read Library commit")
            .expect("Library commit exists")
            .commit,
        commit
    );
    assert_eq!(
        handle
            .edit_repository_ref("heads/main")
            .expect("read Library head")
            .expect("Library head exists")
            .commit_id,
        commit.id()
    );
    assert_eq!(
        handle
            .edit_object(root_id)
            .expect("read Library root")
            .expect("Library root exists")
            .object
            .id(),
        root_id
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_routes_the_atomic_recipe_and_repository_commit() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let photo_id = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/combined-actor.dng".to_vec(),
                "/photos/combined-actor.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 10,
        })
        .expect("register combined-commit photo")
        .photo_id;
    let root = LibraryRootV1 {
        photo_recipes: None,
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .expect("pack empty Library root");
    let root_id = root.object().id();
    handle
        .store_edit_object_pack(&EditObjectPackWrite {
            objects: vec![root],
            created_at_ms: 11,
        })
        .expect("store Library root");

    let recipe = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Atomic actor Recipe".into()),
        12,
    )
    .expect("build Recipe commit");
    let repository = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root: root_id,
        parents: Vec::new(),
        message: Some("Atomic actor Library commit".into()),
        created_at_ms: 12,
    })
    .expect("build Library commit");

    let committed = handle
        .commit_recipe_and_edit_repository(&CommitRecipeAndEditRepository {
            recipe: CommitRecipe {
                photo_id,
                commit: recipe.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            },
            repository: CommitEditRepository {
                commit: repository.clone(),
                update_refs: vec![EditRepositoryRefUpdate {
                    name: "heads/main".into(),
                    kind: EditRepositoryRefKind::Branch,
                    expected: EditRepositoryRefExpectation::Missing,
                    updated_at_ms: 12,
                }],
            },
        })
        .expect("commit both histories through actor");

    assert_eq!(committed.recipe.commit, recipe);
    assert_eq!(committed.repository.commit, repository);
    assert_eq!(
        handle
            .recipe_ref(photo_id, "working")
            .expect("read Recipe head")
            .expect("Recipe head exists")
            .commit_id,
        recipe.id()
    );
    assert_eq!(
        handle
            .edit_repository_ref("heads/main")
            .expect("read Library head")
            .expect("Library head exists")
            .commit_id,
        repository.id()
    );
    actor.shutdown().expect("shutdown actor");
}

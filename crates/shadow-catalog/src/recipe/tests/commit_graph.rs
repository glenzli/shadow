use shadow_domain::{AssetLocation, EntityId, Platform, RecipeId, RepresentationKind};

use super::{
    super::{CommitRecipe, RecipeRefKind, RecipeRefTarget, SetRecipeRef},
    recipe_fixtures::{catalog_with_photo, commit},
};
use crate::{CatalogError, RegisterAsset};

#[test]
fn immutable_commits_branch_without_overwriting_the_previous_head() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/edit.dng");
    let recipe_id = RecipeId::new_v7();
    let root = commit(recipe_id, Vec::new(), "Natural base", 100);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: root.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: None,
            }],
        })
        .expect("commit root");

    let warm = commit(recipe_id, vec![root.id()], "Warm editorial", 200);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: warm.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: None,
            }],
        })
        .expect("commit child");
    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read working ref")
            .expect("working ref")
            .commit_id,
        warm.id()
    );

    catalog
        .set_recipe_ref(&SetRecipeRef {
            photo_id,
            name: "versions/natural-base".into(),
            kind: RecipeRefKind::NamedVersion,
            commit_id: root.id(),
            updated_at_ms: 250,
        })
        .expect("name old version");
    let commits = catalog.recipe_commits(photo_id).expect("list history");
    assert_eq!(commits.len(), 2);
    assert_eq!(commits[0].commit.id(), warm.id());
    assert_eq!(commits[1].commit.id(), root.id());
    assert_eq!(commits[1].commit.message(), Some("Natural base"));
    assert_eq!(commits[0].snapshot_digest, commits[1].snapshot_digest);
}

#[test]
fn commits_cannot_be_replaced_or_parented_across_photos() {
    let (mut catalog, first_photo) = catalog_with_photo("/photos/one.dng");
    let second = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/two.dng".to_vec(),
                "/photos/two.dng",
            ),
            byte_len: 2,
            modified_at_ms: Some(2),
            now_ms: 2,
        })
        .expect("register second photo");
    let recipe_id = RecipeId::new_v7();
    let root = commit(recipe_id, Vec::new(), "Root", 100);
    let request = CommitRecipe {
        photo_id: first_photo,
        commit: root.clone(),
        update_refs: Vec::new(),
    };
    catalog.commit_recipe(&request).expect("commit root");
    assert!(matches!(
        catalog.commit_recipe(&request),
        Err(CatalogError::RecipeCommitAlreadyExists(id)) if id == root.id()
    ));

    let invalid_child = commit(recipe_id, vec![root.id()], "Wrong owner", 200);
    assert!(matches!(
        catalog.commit_recipe(&CommitRecipe {
            photo_id: second.photo_id,
            commit: invalid_child,
            update_refs: Vec::new(),
        }),
        Err(CatalogError::RecipeCommitOwnerMismatch { .. })
    ));
}

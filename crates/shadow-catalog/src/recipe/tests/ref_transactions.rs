use shadow_domain::{EntityId, RecipeId};

use super::{
    super::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget},
    recipe_fixtures::{catalog_with_photo, commit},
};
use crate::CatalogError;

#[test]
fn commit_can_require_a_recipe_ref_to_be_missing() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/missing-ref.dng");
    let root = commit(RecipeId::new_v7(), Vec::new(), "Root", 100);

    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: root.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("create missing ref atomically");

    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read working ref")
            .expect("working ref")
            .commit_id,
        root.id()
    );
}

#[test]
fn commit_can_compare_and_swap_a_recipe_ref_at_its_expected_head() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/expected-head.dng");
    let recipe_id = RecipeId::new_v7();
    let root = commit(recipe_id, Vec::new(), "Root", 100);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: root.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("commit root");
    let child = commit(recipe_id, vec![root.id()], "Child", 200);

    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: child.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::At(root.id())),
            }],
        })
        .expect("advance expected head");

    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read working ref")
            .expect("working ref")
            .commit_id,
        child.id()
    );
}

#[test]
fn stale_recipe_ref_expectation_rolls_back_commit_and_every_ref_move() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/stale-head.dng");
    let recipe_id = RecipeId::new_v7();
    let root = commit(recipe_id, Vec::new(), "Root", 100);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: root.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("commit root");
    let current = commit(recipe_id, vec![root.id()], "Current", 200);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: current.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::At(root.id())),
            }],
        })
        .expect("advance working ref");
    let stale = commit(recipe_id, vec![root.id()], "Stale", 300);

    assert!(matches!(
        catalog.commit_recipe(&CommitRecipe {
            photo_id,
            commit: stale.clone(),
            update_refs: vec![
                RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::At(root.id())),
                },
                RecipeRefTarget {
                    name: "versions/stale".into(),
                    kind: RecipeRefKind::NamedVersion,
                    expectation: None,
                },
            ],
        }),
        Err(CatalogError::RecipeRefExpectationMismatch {
            photo_id: error_photo_id,
            ref name,
            expected: RecipeRefExpectation::At(expected),
            actual: Some(actual),
        }) if error_photo_id == photo_id
            && name == "working"
            && expected == root.id()
            && actual == current.id()
    ));
    assert!(
        catalog
            .recipe_commit(photo_id, stale.id())
            .expect("query stale commit")
            .is_none()
    );
    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read working ref")
            .expect("working ref")
            .commit_id,
        current.id()
    );
    assert!(
        catalog
            .recipe_ref(photo_id, "versions/stale")
            .expect("read untouched secondary ref")
            .is_none()
    );
    assert_eq!(
        catalog
            .recipe_commits(photo_id)
            .expect("list committed history")
            .len(),
        2
    );
}

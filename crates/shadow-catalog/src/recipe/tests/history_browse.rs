use shadow_domain::{EntityId, RecipeCommit, RecipeCommitId, RecipeId, RecipeSnapshot};

use super::{
    super::{
        CommitRecipe, MAX_RECIPE_HISTORY_PAGE_SIZE, RecipeRefExpectation, RecipeRefKind,
        RecipeRefTarget,
    },
    recipe_fixtures::catalog_with_photo,
};

#[test]
fn recipe_history_page_is_keyset_stable_and_projects_refs() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/history-page.dng");
    let recipe_id = RecipeId::new_v7();
    let root = recipe_commit(recipe_id, Vec::new(), Some("Original"), 100);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: root.clone(),
            update_refs: vec![RecipeRefTarget {
                name: format!("versions/{}", root.id()),
                kind: RecipeRefKind::NamedVersion,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .unwrap();
    let autosave = recipe_commit(recipe_id, vec![root.id()], None, 200);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: autosave.clone(),
            update_refs: Vec::new(),
        })
        .unwrap();
    let working = recipe_commit(recipe_id, vec![autosave.id()], None, 300);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: working.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .unwrap();

    let first = catalog.recipe_history_page(photo_id, None, 2).unwrap();
    assert_eq!(
        first
            .entries
            .iter()
            .map(|entry| entry.record.commit.id())
            .collect::<Vec<_>>(),
        vec![working.id(), autosave.id()]
    );
    assert_eq!(first.entries[0].refs.len(), 1);
    assert_eq!(first.entries[0].refs[0].name, "working");
    assert!(first.entries[1].refs.is_empty());

    let newer = recipe_commit(recipe_id, vec![working.id()], None, 400);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: newer,
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::At(working.id())),
            }],
        })
        .unwrap();

    let second = catalog
        .recipe_history_page(photo_id, first.next_cursor.as_ref(), 2)
        .unwrap();
    assert_eq!(second.entries.len(), 1);
    assert_eq!(second.entries[0].record.commit.id(), root.id());
    assert_eq!(second.entries[0].refs.len(), 1);
    assert_eq!(second.entries[0].refs[0].kind, RecipeRefKind::NamedVersion);
    assert!(second.next_cursor.is_none());
}

#[test]
fn recipe_history_page_rejects_unbounded_limits() {
    let (catalog, photo_id) = catalog_with_photo("/photos/history-limit.dng");
    assert!(catalog.recipe_history_page(photo_id, None, 0).is_err());
    assert!(
        catalog
            .recipe_history_page(photo_id, None, MAX_RECIPE_HISTORY_PAGE_SIZE + 1)
            .is_err()
    );
}

fn recipe_commit(
    recipe_id: RecipeId,
    parents: Vec<RecipeCommitId>,
    message: Option<&str>,
    created_at_ms: i64,
) -> RecipeCommit {
    RecipeCommit::new(
        RecipeCommitId::new_v7(),
        recipe_id,
        parents,
        RecipeSnapshot::empty(),
        message.map(str::to_owned),
        created_at_ms,
    )
    .unwrap()
}

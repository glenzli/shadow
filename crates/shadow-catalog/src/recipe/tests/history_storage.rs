use shadow_domain::{
    AssetLocation, EntityId, Platform, RecipeCommitId, RecipeId, RepresentationKind,
};

use super::{
    super::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget},
    recipe_fixtures::{catalog_with_photo, commit},
};
use crate::{Catalog, RegisterAsset};

#[test]
fn development_reset_discards_only_one_photos_recipe_history() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/reset-me.dng");
    let recipe_id = RecipeId::new_v7();
    let root = commit(recipe_id, Vec::new(), "Old development edit", 100);
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
        .expect("persist old working edit");
    let child = commit(recipe_id, vec![root.id()], "Newer development edit", 200);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: child,
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::At(root.id())),
            }],
        })
        .expect("persist child working edit");

    assert_eq!(
        catalog
            .discard_recipe_history(photo_id)
            .expect("discard development history"),
        2
    );
    assert!(
        catalog
            .recipe_commits(photo_id)
            .expect("list discarded history")
            .is_empty()
    );
    assert!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read discarded working ref")
            .is_none()
    );
}

#[test]
fn recipe_history_survives_catalog_close_and_reopen() {
    let root = std::env::temp_dir().join(format!(
        "shadow-recipe-catalog-{}-{}",
        std::process::id(),
        RecipeCommitId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create test root");
    let path = root.join("catalog.sqlite");
    let (photo_id, commit_id, digest) = {
        let mut catalog = Catalog::open(&path).expect("open catalog");
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/reopen.dng".to_vec(),
                    "/photos/reopen.dng",
                ),
                byte_len: 10,
                modified_at_ms: Some(10),
                now_ms: 10,
            })
            .expect("register photo");
        let commit = commit(RecipeId::new_v7(), Vec::new(), "Persistent", 20);
        let record = catalog
            .commit_recipe(&CommitRecipe {
                photo_id: registered.photo_id,
                commit: commit.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: None,
                }],
            })
            .expect("commit Recipe");
        (registered.photo_id, commit.id(), record.snapshot_digest)
    };

    let connection = rusqlite::Connection::open(&path).expect("open persisted recipe directly");
    connection
        .execute(
            "UPDATE recipe_commits SET snapshot_digest = zeroblob(32) WHERE id = ?1",
            [commit_id.as_bytes().as_slice()],
        )
        .expect("simulate a stale non-canonical v1 digest");
    drop(connection);

    let catalog = Catalog::open(&path).expect("reopen catalog");
    let records = catalog.recipe_commits(photo_id).expect("reload commits");
    assert_eq!(records.len(), 1);
    assert_eq!(records[0].commit.id(), commit_id);
    assert_eq!(records[0].snapshot_digest, digest);
    let repaired_digest: Vec<u8> = catalog
        .connection
        .query_row(
            "SELECT snapshot_digest FROM recipe_commits WHERE id = ?1",
            [commit_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .expect("read repaired digest");
    assert_eq!(repaired_digest, digest);
    let direct = catalog
        .recipe_commit(photo_id, commit_id)
        .expect("load one commit")
        .expect("commit exists");
    assert_eq!(direct, records[0]);
    assert!(
        catalog
            .recipe_commit(photo_id, RecipeCommitId::new_v7())
            .expect("query absent commit")
            .is_none()
    );
    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read ref")
            .expect("working ref")
            .commit_id,
        commit_id
    );
    drop(catalog);
    std::fs::remove_dir_all(root).expect("remove test root");
}

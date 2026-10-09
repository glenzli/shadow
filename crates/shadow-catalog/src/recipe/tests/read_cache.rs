//! Repeated reads must keep observing every persisted validation boundary.

use shadow_domain::{EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId};

use super::recipe_fixtures::{catalog_with_photo, commit};
use crate::{Catalog, CatalogError, CommitRecipe, RecipeRefKind, SetRecipeRef};

fn save(catalog: &mut Catalog, photo: PhotoId, value: RecipeCommit) {
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id: photo,
            commit: value,
            update_refs: vec![],
        })
        .unwrap();
}

fn read(catalog: &Catalog, photo: PhotoId, id: RecipeCommitId) -> crate::RecipeCommitRecord {
    catalog.recipe_commit(photo, id).unwrap().unwrap()
}

#[test]
fn warm_reads_preserve_complete_history_order_and_all_read_surfaces() {
    let (mut catalog, photo) = catalog_with_photo("/synthetic/cache-order.dng");
    let recipe = RecipeId::new_v7();
    let first = commit(recipe, vec![], "First", 100);
    let second = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        recipe,
        vec![first.id()],
        first.snapshot().clone(),
        None,
        100,
    )
    .unwrap();
    let third = commit(recipe, vec![second.id()], "Third", 200);
    for item in [&first, &second, &third] {
        save(&mut catalog, photo, item.clone());
    }
    let mut expected = vec![first.id(), second.id()];
    expected.sort_by(|a, b| b.cmp(a));
    expected.insert(0, third.id());
    for _ in 0..3 {
        let all = catalog.recipe_commits(photo).unwrap();
        assert_eq!(
            all.iter().map(|r| r.commit.id()).collect::<Vec<_>>(),
            expected
        );
        let page = catalog.recipe_history_page(photo, None, 10).unwrap();
        assert_eq!(
            page.entries.iter().map(|r| &r.record).collect::<Vec<_>>(),
            all.iter().collect::<Vec<_>>()
        );
        for record in all {
            assert_eq!(read(&catalog, photo, record.commit.id()), record);
        }
    }
    // The database ordering column remains authoritative even after warming.
    catalog
        .connection
        .execute(
            "UPDATE recipe_commits SET created_at_ms = 300 WHERE id = ?1",
            [first.id().as_bytes().as_slice()],
        )
        .unwrap();
    assert_eq!(
        catalog.recipe_commits(photo).unwrap()[0].commit.id(),
        first.id()
    );
    catalog.discard_recipe_history(photo).unwrap();
    assert!(catalog.recipe_commits(photo).unwrap().is_empty());
    assert!(catalog.recipe_commit(photo, first.id()).unwrap().is_none());
}

#[test]
fn warm_reads_reject_changed_json_and_invalid_domain_values() {
    let (mut catalog, photo) = catalog_with_photo("/synthetic/cache-json.dng");
    let value = commit(RecipeId::new_v7(), vec![], "Valid", 100);
    save(&mut catalog, photo, value.clone());
    let original = read(&catalog, photo, value.id());
    let valid_json = serde_json::to_string(&value).unwrap();
    let mut invalid = serde_json::to_value(&value).unwrap();
    invalid["message"] = serde_json::json!("bad\u{0000}message");
    for json in ["{}".to_owned(), serde_json::to_string(&invalid).unwrap()] {
        catalog
            .connection
            .execute(
                "UPDATE recipe_commits SET commit_json = ?1 WHERE id = ?2",
                rusqlite::params![json, value.id().as_bytes().as_slice()],
            )
            .unwrap();
        assert!(catalog.recipe_commit(photo, value.id()).is_err());
        assert!(catalog.recipe_commits(photo).is_err());
        assert!(catalog.recipe_history_page(photo, None, 10).is_err());
        catalog
            .connection
            .execute(
                "UPDATE recipe_commits SET commit_json = ?1 WHERE id = ?2",
                rusqlite::params![&valid_json, value.id().as_bytes().as_slice()],
            )
            .unwrap();
        assert_eq!(read(&catalog, photo, value.id()), original);
    }
    let pretty = serde_json::to_string_pretty(&value).unwrap();
    catalog
        .connection
        .execute(
            "UPDATE recipe_commits SET commit_json = ?1 WHERE id = ?2",
            rusqlite::params![pretty, value.id().as_bytes().as_slice()],
        )
        .unwrap();
    assert_eq!(read(&catalog, photo, value.id()), original);
}

#[test]
fn warm_reads_recheck_indexed_identity() {
    let (mut catalog, photo) = catalog_with_photo("/synthetic/cache-identity.dng");
    let value = commit(RecipeId::new_v7(), vec![], "Valid", 100);
    save(&mut catalog, photo, value.clone());
    read(&catalog, photo, value.id());
    catalog
        .connection
        .execute(
            "UPDATE recipe_commits SET recipe_id = ?1 WHERE id = ?2",
            rusqlite::params![
                RecipeId::new_v7().as_bytes().as_slice(),
                value.id().as_bytes().as_slice()
            ],
        )
        .unwrap();
    assert!(
        matches!(catalog.recipe_commit(photo, value.id()), Err(CatalogError::InvalidRecipe(message)) if message.contains("identity"))
    );
}

#[test]
fn warm_reads_recheck_normalized_parent_edges() {
    let (mut catalog, photo) = catalog_with_photo("/synthetic/cache-parents.dng");
    let recipe = RecipeId::new_v7();
    let first = commit(recipe, vec![], "First", 100);
    let other = commit(recipe, vec![], "Other", 110);
    let child = commit(recipe, vec![first.id()], "Child", 200);
    for item in [&first, &other, &child] {
        save(&mut catalog, photo, item.clone());
    }
    let original = read(&catalog, photo, child.id());
    catalog
        .connection
        .execute(
            "UPDATE recipe_commit_parents SET parent_id = ?1 WHERE commit_id = ?2",
            rusqlite::params![
                other.id().as_bytes().as_slice(),
                child.id().as_bytes().as_slice()
            ],
        )
        .unwrap();
    assert!(
        matches!(catalog.recipe_commit(photo, child.id()), Err(CatalogError::InvalidRecipe(message)) if message.contains("parents"))
    );
    catalog
        .connection
        .execute(
            "UPDATE recipe_commit_parents SET parent_id = ?1 WHERE commit_id = ?2",
            rusqlite::params![
                first.id().as_bytes().as_slice(),
                child.id().as_bytes().as_slice()
            ],
        )
        .unwrap();
    assert_eq!(read(&catalog, photo, child.id()), original);
}

#[test]
fn warm_reads_still_repair_noncanonical_snapshot_digests() {
    let (mut catalog, photo) = catalog_with_photo("/synthetic/cache-digest.dng");
    let value = commit(RecipeId::new_v7(), vec![], "Valid", 100);
    save(&mut catalog, photo, value.clone());
    let original = read(&catalog, photo, value.id());
    catalog
        .connection
        .execute(
            "UPDATE recipe_commits SET snapshot_digest = zeroblob(32) WHERE id = ?1",
            [value.id().as_bytes().as_slice()],
        )
        .unwrap();
    assert_eq!(read(&catalog, photo, value.id()), original);
    let digest: Vec<u8> = catalog
        .connection
        .query_row(
            "SELECT snapshot_digest FROM recipe_commits WHERE id = ?1",
            [value.id().as_bytes().as_slice()],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(digest, original.snapshot_digest);
}

#[test]
fn cold_and_warm_reads_propagate_digest_repair_failure_and_allow_retry() {
    for warm in [false, true] {
        let (mut catalog, photo) = catalog_with_photo("/synthetic/cache-repair-failure.dng");
        let value = commit(RecipeId::new_v7(), vec![], "Valid", 100);
        save(&mut catalog, photo, value.clone());
        if warm {
            read(&catalog, photo, value.id());
        }
        catalog
            .connection
            .execute(
                "UPDATE recipe_commits SET snapshot_digest = zeroblob(32) WHERE id = ?1",
                [value.id().as_bytes().as_slice()],
            )
            .unwrap();
        catalog
            .connection
            .pragma_update(None, "query_only", true)
            .unwrap();
        for result in [
            catalog.recipe_commit(photo, value.id()).map(|_| ()),
            catalog.recipe_commits(photo).map(|_| ()),
            catalog.recipe_history_page(photo, None, 10).map(|_| ()),
        ] {
            assert!(
                matches!(result, Err(CatalogError::Sqlite(rusqlite::Error::SqliteFailure(error, _)))
                if error.code == rusqlite::ErrorCode::ReadOnly)
            );
        }
        let unchanged: Vec<u8> = catalog
            .connection
            .query_row(
                "SELECT snapshot_digest FROM recipe_commits WHERE id = ?1",
                [value.id().as_bytes().as_slice()],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(unchanged, [0; 32]);
        catalog
            .connection
            .pragma_update(None, "query_only", false)
            .unwrap();
        assert_eq!(read(&catalog, photo, value.id()).commit, value);
    }
}

#[test]
fn warm_reads_observe_external_json_membership_and_ref_changes() {
    let root = std::env::temp_dir().join(format!(
        "shadow-cache-external-{}",
        RecipeCommitId::new_v7()
    ));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&path).unwrap();
    let photo = catalog
        .register_asset(&crate::RegisterAsset {
            kind: shadow_domain::RepresentationKind::OriginalRaw,
            location: shadow_domain::AssetLocation::new(
                shadow_domain::Platform::MacOs,
                b"/synthetic/external.dng".to_vec(),
                "/synthetic/external.dng",
            ),
            byte_len: 1,
            modified_at_ms: Some(1),
            now_ms: 1,
        })
        .unwrap()
        .photo_id;
    let value = commit(RecipeId::new_v7(), vec![], "Before", 100);
    save(&mut catalog, photo, value.clone());
    read(&catalog, photo, value.id());
    let mut other = Catalog::open(&path).unwrap();
    let mut changed = serde_json::to_value(&value).unwrap();
    changed["message"] = serde_json::json!("After");
    other
        .connection
        .execute(
            "UPDATE recipe_commits SET commit_json = ?1 WHERE id = ?2",
            rusqlite::params![
                serde_json::to_string(&changed).unwrap(),
                value.id().as_bytes().as_slice()
            ],
        )
        .unwrap();
    assert_eq!(
        read(&catalog, photo, value.id()).commit.message(),
        Some("After")
    );
    let child = commit(value.recipe_id(), vec![value.id()], "External child", 200);
    save(&mut other, photo, child.clone());
    let mut target = SetRecipeRef {
        photo_id: photo,
        name: "external-checkpoint".into(),
        kind: RecipeRefKind::NamedVersion,
        commit_id: child.id(),
        updated_at_ms: 300,
    };
    other.set_recipe_ref(&target).unwrap();
    assert_eq!(catalog.recipe_commits(photo).unwrap().len(), 2);
    let page = catalog.recipe_history_page(photo, None, 10).unwrap();
    assert_eq!(page.entries[0].record.commit, child);
    assert_eq!(page.entries[0].refs[0].name, target.name);
    assert!(page.entries[1].refs.is_empty());
    target.commit_id = value.id();
    target.updated_at_ms = 400;
    other.set_recipe_ref(&target).unwrap();
    let page = catalog.recipe_history_page(photo, None, 10).unwrap();
    assert!(page.entries[0].refs.is_empty());
    assert_eq!(page.entries[1].refs[0].name, target.name);
    assert_eq!(
        catalog
            .recipe_ref(photo, &target.name)
            .unwrap()
            .unwrap()
            .commit_id,
        value.id()
    );
    other.discard_recipe_history(photo).unwrap();
    assert!(catalog.recipe_commits(photo).unwrap().is_empty());
    assert!(
        catalog
            .recipe_history_page(photo, None, 10)
            .unwrap()
            .entries
            .is_empty()
    );
    assert!(catalog.recipe_commit(photo, value.id()).unwrap().is_none());
    assert!(catalog.recipe_commit(photo, child.id()).unwrap().is_none());
    assert!(catalog.recipe_ref(photo, &target.name).unwrap().is_none());
    drop(other);
    drop(catalog);
    std::fs::remove_dir_all(root).unwrap();
}

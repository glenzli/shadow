use shadow_domain::{EntityId, RecipeId, RecipeSnapshot};

use super::{DecodedRecipeCache, PhotoId, RecipeCommit, RecipeCommitId, ValidatedRecipe};

fn value() -> ValidatedRecipe {
    ValidatedRecipe {
        commit: RecipeCommit::new(
            RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            vec![],
            RecipeSnapshot::empty(),
            None,
            1,
        )
        .unwrap(),
        snapshot_digest: [7; 32],
    }
}

#[test]
fn only_exact_bytes_and_photo_commit_identity_hit() {
    let photo = PhotoId::new_v7();
    let value = value();
    let id = value.commit.id();
    let mut cache = DecodedRecipeCache::default();
    cache.insert(
        photo,
        "validated bytes",
        &value.commit,
        value.snapshot_digest,
    );
    assert_eq!(
        cache.get(photo, id, "validated bytes").unwrap().commit,
        value.commit
    );
    assert!(cache.get(photo, id, "changed bytes").is_none());
    assert!(
        cache
            .get(PhotoId::new_v7(), id, "validated bytes")
            .is_none()
    );
    assert!(
        cache
            .get(photo, RecipeCommitId::new_v7(), "validated bytes")
            .is_none()
    );
}

#[test]
fn fifo_entry_budget_removes_the_oldest_admission() {
    let photo = PhotoId::new_v7();
    let mut cache = DecodedRecipeCache::with_limits(2, 100, 100);
    let values = [value(), value(), value()];
    for item in &values {
        cache.insert(photo, "json", &item.commit, item.snapshot_digest);
    }
    assert!(cache.get(photo, values[0].commit.id(), "json").is_none());
    assert!(cache.get(photo, values[1].commit.id(), "json").is_some());
    assert!(cache.get(photo, values[2].commit.id(), "json").is_some());
    assert_eq!(cache.json_bytes, 8);
    assert_eq!(cache.insertion_order.len(), 2);
}

#[test]
fn byte_budget_and_oversized_admissions_preserve_bounded_state() {
    let photo = PhotoId::new_v7();
    let mut cache = DecodedRecipeCache::with_limits(10, 7, 4);
    let first = value();
    let second = value();
    let large = value();
    cache.insert(photo, "1234", &first.commit, first.snapshot_digest);
    cache.insert(photo, "5678", &second.commit, second.snapshot_digest);
    assert!(cache.get(photo, first.commit.id(), "1234").is_none());
    assert!(cache.get(photo, second.commit.id(), "5678").is_some());
    cache.insert(photo, "large", &large.commit, large.snapshot_digest);
    assert!(cache.get(photo, large.commit.id(), "large").is_none());
    assert_eq!(cache.json_bytes, 4);
    assert_eq!(cache.entries.len(), 1);
}

#[test]
fn replacement_reaccounts_bytes_without_duplicate_queue_entries() {
    let photo = PhotoId::new_v7();
    let item = value();
    let mut cache = DecodedRecipeCache::with_limits(2, 10, 10);
    cache.insert(photo, "old", &item.commit, item.snapshot_digest);
    for _ in 0..10 {
        cache.insert(photo, "new bytes", &item.commit, item.snapshot_digest);
    }
    assert_eq!(cache.entries.len(), 1);
    assert_eq!(cache.insertion_order.len(), 1);
    assert_eq!(cache.json_bytes, 9);
    assert!(cache.get(photo, item.commit.id(), "old").is_none());
    assert!(cache.get(photo, item.commit.id(), "new bytes").is_some());
    cache.insert(
        photo,
        "too large to retain",
        &item.commit,
        item.snapshot_digest,
    );
    assert!(cache.entries.is_empty());
    assert!(cache.insertion_order.is_empty());
    assert_eq!(cache.json_bytes, 0);
}

#[test]
fn owned_return_values_cannot_modify_cached_state() {
    let photo = PhotoId::new_v7();
    let item = value();
    let mut cache = DecodedRecipeCache::default();
    cache.insert(photo, "json", &item.commit, item.snapshot_digest);
    let mut returned = cache.get(photo, item.commit.id(), "json").unwrap();
    returned.snapshot_digest = [0; 32];
    assert_eq!(
        cache
            .get(photo, item.commit.id(), "json")
            .unwrap()
            .snapshot_digest,
        [7; 32]
    );
}

#[test]
fn scans_larger_than_capacity_remain_bounded_without_assuming_hits() {
    let photo = PhotoId::new_v7();
    for mut cache in [
        DecodedRecipeCache::with_limits(2, 100, 100),
        DecodedRecipeCache::with_limits(100, 8, 100),
    ] {
        let values = [value(), value(), value()];
        for _ in 0..3 {
            let mut hits = 0;
            for item in &values {
                if cache.get(photo, item.commit.id(), "json").is_some() {
                    hits += 1;
                } else {
                    cache.insert(photo, "json", &item.commit, item.snapshot_digest);
                }
            }
            assert_eq!(hits, 0, "FIFO scan beyond capacity cannot reuse entries");
            assert_eq!(cache.entries.len(), 2);
            assert_eq!(cache.json_bytes, 8);
            assert_eq!(cache.insertion_order.len(), 2);
        }
    }
}

#[test]
fn disabled_or_smaller_than_one_entry_budgets_skip_admission() {
    let photo = PhotoId::new_v7();
    let item = value();
    for mut cache in [
        DecodedRecipeCache::with_limits(0, 100, 100),
        DecodedRecipeCache::with_limits(10, 0, 100),
        DecodedRecipeCache::with_limits(10, 100, 0),
    ] {
        cache.insert(photo, "json", &item.commit, item.snapshot_digest);
        assert!(cache.entries.is_empty());
        assert!(cache.insertion_order.is_empty());
        assert_eq!(cache.json_bytes, 0);
    }
}

//! Explicit, bounded measurements of synthetic edit-history projection.

use std::time::Instant;

use shadow_catalog::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget};
use shadow_domain::{EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId};

use crate::{
    recipe_v1::{decode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot},
    session_edit_history::WORKING_RECIPE_REF,
    tests::fixtures::{edit_session::test_edit_session, grade_stack::ffi_parameters},
};

#[test]
#[ignore = "synthetic history timing; run alone with --ignored --nocapture"]
fn measure_history_projection_at_increasing_commit_counts() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let initial = session.photo_edit_state(&photo_id, &source_path).unwrap();
    let variant = initial.active_variant_id.parse().unwrap();
    let photo: PhotoId = photo_id.parse().unwrap();
    let recipe = RecipeId::new_v7();
    let draft =
        decode_grade_stack_draft_recipe_v1(&ffi_parameters(0.25, 1.0, [0.0; 2], 1.0)).unwrap();
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).unwrap();
    let mut parent: Option<RecipeCommitId> = None;
    let mut previous_count = 0;
    for count in [64, 256, 1_024] {
        let seed_start = Instant::now();
        for index in previous_count..count {
            let id = RecipeCommitId::new_v7();
            session
                .catalog
                .commit_recipe_for_variant(
                    &CommitRecipe {
                        photo_id: photo,
                        commit: RecipeCommit::new(
                            id,
                            recipe,
                            parent.into_iter().collect(),
                            snapshot.clone(),
                            ((index + 1) % 32 == 0).then(|| format!("Checkpoint {}", index + 1)),
                            i64::from(index),
                        )
                        .unwrap(),
                        update_refs: vec![RecipeRefTarget {
                            name: WORKING_RECIPE_REF.to_owned(),
                            kind: RecipeRefKind::Working,
                            expectation: Some(
                                parent.map_or(
                                    RecipeRefExpectation::Missing,
                                    RecipeRefExpectation::At,
                                ),
                            ),
                        }],
                    },
                    variant,
                )
                .unwrap();
            parent = Some(id);
        }
        let seed_ms = seed_start.elapsed().as_secs_f64() * 1_000.0;
        let mut projection_ms = Vec::new();
        for _ in 0..3 {
            let start = Instant::now();
            let state = session.photo_edit_state(&photo_id, &source_path).unwrap();
            projection_ms.push(start.elapsed().as_secs_f64() * 1_000.0);
            assert_eq!(state.working_commit_id, parent.unwrap().to_string());
            assert_eq!(state.versions.len(), usize::try_from(count / 32).unwrap());
        }
        println!(
            "HISTORY_SCALE {}",
            serde_json::json!({
                "commits": count,
                "named_versions": count / 32,
                "new_commits_seeded": count - previous_count,
                "seed_ms": seed_ms,
                "projection_ms": projection_ms,
                "scope": "Debug synthetic Catalog-backed photo_edit_state; no render or model calls"
            })
        );
        previous_count = count;
    }
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}

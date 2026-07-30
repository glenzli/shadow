use shadow_catalog::{
    CatalogActor, CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget, RegisterAsset,
};
use shadow_domain::{
    AssetLocation, EntityId, PhotoFoundationNode, Platform, RawCameraNeutral, RawWhiteBalance,
    RecipeCommit, RecipeCommitId, RecipeId, RecipeInputSettings, RecipeOpticsSettings,
    RepresentationKind,
};

use super::*;
use crate::recipe_v1::{
    GradeStackDraft, encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
};

#[test]
#[allow(clippy::too_many_lines)] // One queued-base race contract is intentionally end to end.
fn queued_render_uses_its_explicit_base_after_the_working_ref_moves() {
    let root = std::env::temp_dir().join(format!(
        "shadow-recipe-render-request-{}-{}",
        std::process::id(),
        RecipeCommitId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create render-request fixture");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("open fixture Catalog");
    let catalog = actor.handle();
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/render-request.dng".to_vec(),
                "/photos/render-request.dng",
            ),
            byte_len: 4_096,
            modified_at_ms: Some(100),
            now_ms: 100,
        })
        .expect("register render-request photo");

    let manual_white_balance = RawWhiteBalance::camera_neutral(
        RawCameraNeutral::from_millionths(875_000, 1_250_000).expect("manual camera neutral"),
    );
    let base_draft = GradeStackDraft {
        foundation: PhotoFoundationNode::new(
            RecipeInputSettings::new(RecipeOpticsSettings::default())
                .with_raw_white_balance(manual_white_balance),
        ),
        ..GradeStackDraft::default()
    };
    let base_snapshot =
        grade_stack_recipe_v1_snapshot(&base_draft, None).expect("build queued base Recipe");
    let recipe_id = RecipeId::new_v7();
    let base_commit_id = RecipeCommitId::new_v7();
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id: registered.photo_id,
            commit: RecipeCommit::new(
                base_commit_id,
                recipe_id,
                Vec::new(),
                base_snapshot.clone(),
                Some("Queued base".to_owned()),
                200,
            )
            .expect("build queued base commit"),
            update_refs: vec![RecipeRefTarget {
                name: "working".to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("persist queued base");

    let newer_snapshot = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
        .expect("build newer working Recipe");
    let newer_commit_id = RecipeCommitId::new_v7();
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id: registered.photo_id,
            commit: RecipeCommit::new(
                newer_commit_id,
                recipe_id,
                vec![base_commit_id],
                newer_snapshot,
                Some("Newer working Recipe".to_owned()),
                300,
            )
            .expect("build newer working commit"),
            update_refs: vec![RecipeRefTarget {
                name: "working".to_owned(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::At(base_commit_id)),
            }],
        })
        .expect("move working ref");

    let mut queued_draft = base_draft;
    queued_draft.basic.exposure_stops = 1.25;
    let expected_snapshot = grade_stack_recipe_v1_snapshot(&queued_draft, Some(&base_snapshot))
        .expect("build expected queued Recipe");
    let expected_digest = shadow_domain::canonical_recipe_snapshot_digest(&expected_snapshot)
        .expect("identify expected queued Recipe");
    let settings = encode_grade_stack_draft_recipe_v1(queued_draft).expect("encode Grade Stack");

    let resolved = resolve_recipe_render(
        &catalog,
        registered.photo_id,
        &base_commit_id.to_string(),
        &settings,
        true,
    )
    .expect("resolve queued Recipe");

    assert_eq!(resolved.snapshot_digest, expected_digest);
    assert_eq!(resolved.raw_white_balance, manual_white_balance);
    assert_eq!(
        catalog
            .recipe_ref(registered.photo_id, "working")
            .expect("read moved working ref")
            .expect("working ref exists")
            .commit_id,
        newer_commit_id
    );

    drop(catalog);
    actor.shutdown().expect("close fixture Catalog");
    std::fs::remove_dir_all(root).expect("remove render-request fixture");
}

use shadow_catalog::{Catalog, CommitRecipe, RecipeRefKind, RecipeRefTarget, RegisterAsset};
use shadow_domain::{
    AssetLocation, EntityId, NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoDecisionState,
    PhotoFlag, Platform, RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind,
};

#[test]
fn review_source_and_page_join_the_pointer_only_decision_projection() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/decision-join.dng".to_vec(),
                "/photos/decision-join.dng",
            ),
            byte_len: 4_096,
            modified_at_ms: Some(123),
            now_ms: 100,
        })
        .expect("register decision Review source");
    assert_eq!(
        catalog
            .review_source(registered.photo_id)
            .expect("read default Review source")
            .expect("default Review source")
            .decision,
        PhotoDecisionState::default()
    );
    assert_eq!(
        catalog
            .review_page(None, 10)
            .expect("read default Review page")
            .items[0]
            .decision,
        PhotoDecisionState::default()
    );
    assert!(
        !catalog
            .review_source(registered.photo_id)
            .expect("read untouched Review source")
            .expect("untouched Review source")
            .has_development_edits
    );

    let recipe = RecipeCommit::new(
        shadow_domain::RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("working edit".into()),
        200,
    )
    .expect("build working Recipe");
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id: registered.photo_id,
            commit: recipe,
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: None,
            }],
        })
        .expect("persist working Recipe");
    assert!(
        catalog
            .review_page(None, 10)
            .expect("read edited Review page")
            .items[0]
            .has_development_edits
    );

    let event = catalog
        .append_photo_decision_event(&NewPhotoDecisionEvent {
            event_id: "review-decision-join".into(),
            photo_id: registered.photo_id,
            occurred_at_unix_ms: 1_700_000_001_000,
            origin: PhotoDecisionOrigin::Human,
            expected_head_sequence: 0,
            before_flag: PhotoFlag::Unflagged,
            before_rating: 0,
            after_flag: PhotoFlag::Picked,
            after_rating: 4,
        })
        .expect("append Review decision");
    let expected = event.after_state().expect("decision state");
    assert_eq!(
        catalog
            .review_source(registered.photo_id)
            .expect("read decided Review source")
            .expect("decided Review source")
            .decision,
        expected
    );
    assert_eq!(
        catalog
            .review_page(None, 10)
            .expect("read decided Review page")
            .items[0]
            .decision,
        expected
    );
}

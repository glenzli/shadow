use super::*;
use crate::recipe::test_support::{exposure_graph, inline_layer};
use crate::recipe::{
    AdjustmentScope, BlendMode, BranchName, LayerContent, LayerInstance, LayerRevisionSelector,
    RecipeSnapshot, RecipeValidationError, UnitInterval, VersionName,
};
use crate::{
    BranchId, EntityId, LayerId, LayerInstanceId, RecipeCommitId, RecipeId, SelectionId, VersionId,
};

#[test]
fn working_recipe_may_follow_head_but_commit_must_pin_it() {
    let instance_id = LayerInstanceId::new_v7();
    let layer = LayerInstance::new(
        instance_id,
        "Live shared look",
        AdjustmentScope::Selection(SelectionId::new_v7()),
        LayerContent::Shared {
            layer_id: LayerId::new_v7(),
            revision: LayerRevisionSelector::FollowHead,
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect("valid working layer");
    let snapshot = RecipeSnapshot::new(1, vec![layer]).expect("valid working recipe");

    let error = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        snapshot,
        None,
        1_721_500_000_000,
    )
    .expect_err("commit must not follow a moving head");
    assert_eq!(
        error,
        RecipeValidationError::UnresolvedSharedLayer(instance_id)
    );
}

#[test]
fn commit_branch_and_named_version_round_trip_as_one_history() {
    let recipe_id = RecipeId::new_v7();
    let root_id = RecipeCommitId::new_v7();
    let child_id = RecipeCommitId::new_v7();
    let root = RecipeCommit::new(
        root_id,
        recipe_id,
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Original".into()),
        1_721_500_000_000,
    )
    .expect("valid root commit");
    let child = RecipeCommit::new(
        child_id,
        recipe_id,
        vec![root_id],
        RecipeSnapshot::new(1, vec![inline_layer()]).expect("valid edited recipe"),
        Some("Natural base".into()),
        1_721_500_100_000,
    )
    .expect("valid child commit");
    let branch = RecipeBranch::new(
        BranchId::new_v7(),
        recipe_id,
        BranchName::new("main").expect("valid branch name"),
        child_id,
    );
    let version = NamedVersion::new(
        VersionId::new_v7(),
        recipe_id,
        VersionName::new("Natural Base").expect("valid version name"),
        child_id,
        1_721_500_100_000,
    );
    let history = RecipeHistory::new(recipe_id, vec![child, root], vec![branch], vec![version])
        .expect("valid history");

    let encoded = serde_json::to_string(&history).expect("serialize history");
    let decoded: RecipeHistory = serde_json::from_str(&encoded).expect("deserialize history");
    decoded
        .validate()
        .expect("round-tripped history remains valid");
    assert_eq!(history, decoded);
    assert_eq!(decoded.commits()[0].message(), Some("Natural base"));
}

#[test]
fn history_rejects_unknown_parents_and_duplicate_ref_names() {
    let recipe_id = RecipeId::new_v7();
    let root_id = RecipeCommitId::new_v7();
    let root = RecipeCommit::new(
        root_id,
        recipe_id,
        Vec::new(),
        RecipeSnapshot::empty(),
        None,
        0,
    )
    .expect("root");
    let child = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        recipe_id,
        vec![RecipeCommitId::new_v7()],
        RecipeSnapshot::empty(),
        None,
        1,
    )
    .expect("locally valid child");
    assert!(matches!(
        RecipeHistory::new(recipe_id, vec![root.clone(), child], Vec::new(), Vec::new()),
        Err(RecipeValidationError::UnknownCommitParent { .. })
    ));

    let name = BranchName::new("main").expect("name");
    let branches = vec![
        RecipeBranch::new(BranchId::new_v7(), recipe_id, name.clone(), root_id),
        RecipeBranch::new(BranchId::new_v7(), recipe_id, name.clone(), root_id),
    ];
    assert_eq!(
        RecipeHistory::new(recipe_id, vec![root], branches, Vec::new()),
        Err(RecipeValidationError::DuplicateBranchName(name))
    );
}

#[test]
fn history_rejects_a_commit_cycle() {
    let recipe_id = RecipeId::new_v7();
    let root_id = RecipeCommitId::new_v7();
    let first_id = RecipeCommitId::new_v7();
    let second_id = RecipeCommitId::new_v7();
    let root = RecipeCommit::new(
        root_id,
        recipe_id,
        Vec::new(),
        RecipeSnapshot::empty(),
        None,
        0,
    )
    .expect("root");
    let first = RecipeCommit::new(
        first_id,
        recipe_id,
        vec![second_id],
        RecipeSnapshot::empty(),
        None,
        1,
    )
    .expect("locally valid");
    let second = RecipeCommit::new(
        second_id,
        recipe_id,
        vec![first_id],
        RecipeSnapshot::empty(),
        None,
        2,
    )
    .expect("locally valid");

    assert!(matches!(
        RecipeHistory::new(recipe_id, vec![root, first, second], Vec::new(), Vec::new()),
        Err(RecipeValidationError::CommitCycle(_))
    ));
}

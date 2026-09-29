use shadow_domain::{
    EntityId, ManagedRasterMask, MaskComponentId, MaskComponentOperation, MaskDefinition,
    RasterMaskEncoding, SemanticMaskAggregation, SemanticMaskIntent, ShadowRecipeDocument,
};

use super::{RecipeImportItemTerminal, RecipeImportService, RecipeImportTargetIdentity};
use crate::{
    recipe_import_plan::{RecipeImportPlan, SemanticLeafResolution},
    recipe_v1::{
        CompositeMaskDraft, GradeStackDraft, MaskComponentDraft, MaskComponentDraftDefinition,
        grade_stack_recipe_v1_snapshot,
    },
};

#[test]
fn retry_keeps_completed_items_and_rejects_late_attempts() {
    let service = RecipeImportService::default();
    let snapshot = service
        .insert(target("working-a"), semantic_plan('a'), 11)
        .expect("insert plan");
    let item_id = snapshot.items[0].leaf.item_id.to_string();
    let first = service
        .begin_item(snapshot.plan_token, &item_id, 21)
        .expect("begin item");
    assert_eq!(first.input_session_token, 11);
    assert!(
        service
            .begin_item(snapshot.plan_token, &item_id, 22)
            .is_err()
    );
    service
        .complete_item(
            &first,
            RecipeImportItemTerminal::Unavailable,
            "provider unavailable".into(),
            None,
            None,
        )
        .expect("publish failure");

    let retry = service
        .begin_item(snapshot.plan_token, &item_id, 23)
        .expect("retry item");
    assert!(retry.generation > first.generation);
    assert!(
        !service
            .complete_item(
                &first,
                RecipeImportItemTerminal::Cancelled,
                String::new(),
                None,
                None,
            )
            .expect("late completion is discarded")
    );
    assert_eq!(
        service
            .snapshot(snapshot.plan_token)
            .expect("snapshot")
            .items[0]
            .terminal,
        RecipeImportItemTerminal::Running
    );
}

#[test]
fn target_identity_conflict_and_registry_bound_fail_closed() {
    let service = RecipeImportService::default();
    let first = service
        .insert(target("working-a"), semantic_plan('a'), 1)
        .expect("first plan");
    assert!(
        service
            .finalize(first.plan_token, &target("working-b"))
            .expect_err("changed working identity")
            .to_string()
            .contains("identity changed")
    );

    let seeds = ['b', 'c', 'd', 'e', 'f', '1', '2'];
    for (index, seed) in seeds.into_iter().enumerate() {
        service
            .insert(
                target(&format!("working-{index}")),
                semantic_plan(seed),
                index as u64 + 1,
            )
            .expect("bounded plan");
    }
    assert!(
        service
            .insert(target("overflow"), semantic_plan('1'), 99)
            .expect_err("registry bound")
            .to_string()
            .contains("too many active")
    );
}

#[test]
fn close_returns_every_live_resource_and_removes_plan() {
    let service = RecipeImportService::default();
    let snapshot = service
        .insert(target("working-a"), semantic_plan('a'), 31)
        .expect("insert plan");
    let item_id = snapshot.items[0].leaf.item_id.to_string();
    service
        .begin_item(snapshot.plan_token, &item_id, 41)
        .expect("begin item");
    let resources = service.close(snapshot.plan_token).expect("close plan");
    assert_eq!(resources.input_session_token, 31);
    assert_eq!(resources.job_tokens, vec![41]);
    assert!(resources.proposal_tokens.is_empty());
    assert!(service.snapshot(snapshot.plan_token).is_err());
}

#[test]
fn one_input_session_is_reused_across_items_and_completed_plan_finalizes() {
    let service = RecipeImportService::default();
    let destination = target("working-a");
    let snapshot = service
        .insert(destination.clone(), semantic_plan_two(), 51)
        .expect("insert plan");
    assert_eq!(snapshot.items.len(), 2);
    let first_id = snapshot.items[0].leaf.item_id.to_string();
    let second_id = snapshot.items[1].leaf.item_id.to_string();
    let first = service
        .begin_item(snapshot.plan_token, &first_id, 61)
        .expect("first item");
    service
        .complete_item(
            &first,
            RecipeImportItemTerminal::Staged,
            String::new(),
            Some(71),
            None,
        )
        .expect("stage first");
    resolve_staged(&service, snapshot.plan_token, &first_id, 71, &first, 'c');

    let second = service
        .begin_item(snapshot.plan_token, &second_id, 62)
        .expect("second item");
    assert_eq!(first.input_session_token, second.input_session_token);
    assert_eq!(second.input_session_token, 51);
    service
        .complete_item(
            &second,
            RecipeImportItemTerminal::Staged,
            String::new(),
            Some(72),
            None,
        )
        .expect("stage second");
    resolve_staged(&service, snapshot.plan_token, &second_id, 72, &second, 'd');

    let finalized = service
        .finalize(snapshot.plan_token, &destination)
        .expect("complete final projection");
    assert_eq!(
        finalized.grade_nodes[0]
            .composite_mask
            .as_ref()
            .expect("composite")
            .components
            .len(),
        2
    );
}

fn target(working: &str) -> RecipeImportTargetIdentity {
    RecipeImportTargetIdentity {
        photo_id: "photo-a".into(),
        source_path: "/photos/a.raw".into(),
        base_commit_id: "base-a".into(),
        expected_working_commit_id: working.into(),
        settings: GradeStackDraft::default(),
    }
}

fn semantic_plan(seed: char) -> RecipeImportPlan {
    let mut draft = GradeStackDraft::default();
    let hash = seed.to_string().repeat(64);
    let raster = ManagedRasterMask::new(
        format!("objects/v1/b3/{}/{}", &hash[..2], &hash[2..]),
        1,
        hash,
        16,
        4,
        4,
        4,
        4,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    let intent = SemanticMaskIntent::new("the subject", 4, 30, SemanticMaskAggregation::Union)
        .expect("intent");
    draft.grade_nodes[0].local_mask = Some(
        MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
            raster,
            Some(intent),
            0,
            12,
            false,
        )
        .expect("semantic mask"),
    );
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).expect("Recipe snapshot");
    let document = ShadowRecipeDocument::new(None, snapshot).expect("Recipe document");
    RecipeImportPlan::from_document(&document).expect("import plan")
}

fn semantic_plan_two() -> RecipeImportPlan {
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].composite_mask = Some(CompositeMaskDraft {
        components: vec![
            semantic_component(MaskComponentOperation::Base, 'a'),
            semantic_component(MaskComponentOperation::Subtract, 'b'),
        ],
        invert: false,
    });
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).expect("Recipe snapshot");
    let document = ShadowRecipeDocument::new(None, snapshot).expect("Recipe document");
    RecipeImportPlan::from_document(&document).expect("import plan")
}

fn semantic_component(operation: MaskComponentOperation, seed: char) -> MaskComponentDraft {
    MaskComponentDraft {
        id: MaskComponentId::new_v7(),
        operation,
        enabled: true,
        definition: MaskComponentDraftDefinition::Definition(semantic_definition(seed)),
    }
}

fn semantic_definition(seed: char) -> MaskDefinition {
    let hash = seed.to_string().repeat(64);
    let raster = ManagedRasterMask::new(
        format!("objects/v1/b3/{}/{}", &hash[..2], &hash[2..]),
        1,
        hash,
        16,
        4,
        4,
        4,
        4,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
        raster,
        Some(
            SemanticMaskIntent::new("the subject", 4, 30, SemanticMaskAggregation::Union)
                .expect("intent"),
        ),
        0,
        12,
        false,
    )
    .expect("semantic definition")
}

fn resolve_staged(
    service: &RecipeImportService,
    plan_token: u64,
    item_id: &str,
    proposal_token: u64,
    context: &super::RecipeImportExecutionContext,
    seed: char,
) {
    let leaf = service
        .staged_item(plan_token, item_id, proposal_token, context.generation)
        .expect("staged leaf");
    service
        .resolve_item(
            plan_token,
            item_id,
            proposal_token,
            context.generation,
            SemanticLeafResolution {
                item_id: leaf.item_id,
                grade_node_id: leaf.grade_node_id,
                component_id: leaf.component_id,
                definition: semantic_definition(seed),
            },
        )
        .expect("resolve item");
}

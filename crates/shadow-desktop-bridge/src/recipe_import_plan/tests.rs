use shadow_domain::{
    EntityId, LayerInstanceId, ManagedRasterMask, MaskComponentId, MaskComponentOperation,
    MaskDefinition, RasterMaskEncoding, SemanticMaskAggregation, SemanticMaskIntent,
    ShadowRecipeDocument, UnitInterval,
};

use super::{
    MAX_RECIPE_IMPORT_COMPONENTS, RecipeImportPlan, SemanticLeafResolution, checked_component_total,
};
use crate::recipe_v1::{
    CompositeMaskDraft, GradeNodeDraft, GradeStackDraft, MaskComponentDraft,
    MaskComponentDraftDefinition, grade_stack_recipe_v1_snapshot,
};

#[test]
fn mixed_composite_preserves_all_operations_and_resolves_multiple_semantic_leaves() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].composite_mask = Some(CompositeMaskDraft {
        components: vec![
            portable_component(MaskComponentOperation::Base, radial_mask()),
            managed_component(
                MaskComponentOperation::Add,
                semantic_mask("the sky", -3, 12, false, 'a'),
            ),
            managed_component(
                MaskComponentOperation::Subtract,
                semantic_mask("the main person", 4, 21, true, 'b'),
            ),
            portable_component(MaskComponentOperation::Intersect, luminance_mask()),
        ],
        invert: true,
    });
    let document = document(source);

    let mut plan = RecipeImportPlan::from_document(&document).expect("import plan");

    assert_eq!(plan.label(), "Portable look");
    assert_eq!(plan.pending_semantic_leaves().len(), 2);
    assert_eq!(plan.node_summaries()[0].semantic_leaf_count, 2);
    assert_eq!(plan.node_summaries()[0].unsupported_managed_leaf_count, 0);
    let planned = plan.nodes[0].mask.as_ref().expect("planned mask");
    assert!(planned.invert);
    assert_eq!(
        planned
            .components
            .iter()
            .map(|component| component.operation)
            .collect::<Vec<_>>(),
        vec![
            MaskComponentOperation::Base,
            MaskComponentOperation::Add,
            MaskComponentOperation::Subtract,
            MaskComponentOperation::Intersect,
        ]
    );
    assert!(plan.finalize().is_err());

    for (index, item) in plan
        .pending_semantic_leaves()
        .to_vec()
        .into_iter()
        .enumerate()
    {
        plan.resolve_item(SemanticLeafResolution {
            item_id: item.item_id,
            grade_node_id: item.grade_node_id,
            component_id: item.component_id,
            definition: target_semantic_mask(&item, char::from(b'c' + index as u8)),
        })
        .expect("resolve semantic item");
    }

    assert!(plan.pending_semantic_leaves().is_empty());
    let finalized = plan.finalize().expect("complete plan");
    let composite = finalized.grade_nodes[0]
        .composite_mask
        .as_ref()
        .expect("final composite");
    assert_eq!(composite.components.len(), 4);
    assert!(matches!(
        composite.components[1].definition,
        MaskComponentDraftDefinition::Definition(MaskDefinition::ManagedRaster { .. })
    ));
    assert!(matches!(
        composite.components[2].definition,
        MaskComponentDraftDefinition::Definition(MaskDefinition::ManagedRaster { .. })
    ));
}

#[test]
fn source_raster_and_source_graph_identities_never_enter_plan_projection() {
    let mut source = GradeStackDraft::default();
    let source_node_id = source.grade_nodes[0].recipe_v1_identity.grade_node_id;
    let source_component_id = MaskComponentId::new_v7();
    let (source_raster, source_object_id, source_hash) = managed_raster('e');
    source.grade_nodes[0].composite_mask = Some(CompositeMaskDraft {
        components: vec![MaskComponentDraft {
            id: source_component_id,
            operation: MaskComponentOperation::Base,
            enabled: true,
            definition: MaskComponentDraftDefinition::Definition(
                MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
                    source_raster,
                    Some(intent("the train")),
                    -2,
                    14,
                    false,
                )
                .expect("semantic source mask"),
            ),
        }],
        invert: false,
    });

    let plan = RecipeImportPlan::from_document(&document(source)).expect("import plan");
    let item = &plan.pending_semantic_leaves()[0];

    assert_ne!(plan.nodes[0].grade_node_id, source_node_id);
    assert_ne!(item.component_id, source_component_id);
    let debug = format!("{plan:?}");
    let projection = plan.test_projection_json();
    for forbidden in [
        source_object_id,
        source_hash,
        source_node_id.to_string(),
        source_component_id.to_string(),
    ] {
        assert!(!debug.contains(&forbidden));
        assert!(!projection.contains(&forbidden));
    }
}

#[test]
fn resolution_rejects_stale_identity_duplicate_and_wrong_refinement() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].local_mask = Some(semantic_mask("the bridge", 2, 18, false, '1'));
    let mut plan = RecipeImportPlan::from_document(&document(source)).expect("import plan");
    let item = plan.pending_semantic_leaves()[0].clone();

    let stale = plan
        .resolve_item(SemanticLeafResolution {
            item_id: item.item_id,
            grade_node_id: LayerInstanceId::new_v7(),
            component_id: item.component_id,
            definition: target_semantic_mask(&item, '2'),
        })
        .expect_err("stale node identity");
    assert!(stale.to_string().contains("identity"));
    let mut wrong = target_semantic_mask(&item, '3');
    if let MaskDefinition::ManagedRaster {
        feather_percent, ..
    } = &mut wrong
    {
        *feather_percent += 1;
    }
    let mismatch = plan
        .resolve_item(SemanticLeafResolution {
            item_id: item.item_id,
            grade_node_id: item.grade_node_id,
            component_id: item.component_id,
            definition: wrong,
        })
        .expect_err("wrong refinement");
    assert!(mismatch.to_string().contains("intent and refinement"));

    let resolution = SemanticLeafResolution {
        item_id: item.item_id,
        grade_node_id: item.grade_node_id,
        component_id: item.component_id,
        definition: target_semantic_mask(&item, '4'),
    };
    plan.resolve_item(resolution.clone())
        .expect("current resolution");
    let duplicate = plan
        .resolve_item(resolution)
        .expect_err("duplicate resolution");
    assert!(duplicate.to_string().contains("already resolved"));
}

#[test]
fn unsupported_node_must_be_explicitly_excluded_and_remaining_order_is_stable() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].label = "First".into();
    source.grade_nodes[0].local_mask = Some(radial_mask());
    let mut unsupported = GradeNodeDraft::neutral("Unsupported");
    unsupported.local_mask = Some(nonsemantic_mask('5'));
    let mut last = GradeNodeDraft::neutral("Last");
    last.local_mask = Some(luminance_mask());
    source.grade_nodes.extend([unsupported, last]);

    let mut plan = RecipeImportPlan::from_document(&document(source)).expect("import plan");
    let summaries = plan.node_summaries();
    assert_eq!(
        summaries
            .iter()
            .map(|node| node.label.as_str())
            .collect::<Vec<_>>(),
        vec!["First", "Unsupported", "Last"]
    );
    assert_eq!(summaries[1].unsupported_managed_leaf_count, 1);
    assert!(plan.finalize().is_err());

    plan.exclude_node(summaries[1].grade_node_id)
        .expect("exclude unsupported node");
    let finalized = plan.finalize().expect("partial whole-node projection");
    assert_eq!(
        finalized
            .grade_nodes
            .iter()
            .map(|node| node.label.as_str())
            .collect::<Vec<_>>(),
        vec!["First", "Last"]
    );
    assert!(plan.exclude_node(LayerInstanceId::new_v7()).is_err());
}

#[test]
fn sixteen_nodes_with_eight_components_each_reach_but_do_not_exceed_the_bound() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes.clear();
    for node_index in 0..16 {
        let mut node = GradeNodeDraft::neutral(format!("Node {node_index}"));
        node.composite_mask = Some(CompositeMaskDraft {
            components: (0..8)
                .map(|component_index| {
                    portable_component(
                        if component_index == 0 {
                            MaskComponentOperation::Base
                        } else {
                            MaskComponentOperation::Add
                        },
                        radial_mask(),
                    )
                })
                .collect(),
            invert: false,
        });
        source.grade_nodes.push(node);
    }

    let plan = RecipeImportPlan::from_document(&document(source)).expect("128 component plan");
    assert_eq!(
        plan.nodes
            .iter()
            .map(|node| node.mask.as_ref().expect("mask").components.len())
            .sum::<usize>(),
        MAX_RECIPE_IMPORT_COMPONENTS
    );
    assert!(checked_component_total(MAX_RECIPE_IMPORT_COMPONENTS, 1).is_err());
}

fn document(draft: GradeStackDraft) -> ShadowRecipeDocument {
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).expect("source snapshot");
    ShadowRecipeDocument::new(Some("Portable look"), snapshot).expect("Recipe document")
}

fn portable_component(
    operation: MaskComponentOperation,
    definition: MaskDefinition,
) -> MaskComponentDraft {
    MaskComponentDraft {
        id: MaskComponentId::new_v7(),
        operation,
        enabled: true,
        definition: MaskComponentDraftDefinition::Definition(definition),
    }
}

fn managed_component(
    operation: MaskComponentOperation,
    definition: MaskDefinition,
) -> MaskComponentDraft {
    portable_component(operation, definition)
}

fn radial_mask() -> MaskDefinition {
    MaskDefinition::radial_gradient(unit(0.5), unit(0.5), unit(0.3), unit(0.2), unit(0.4), false)
        .expect("radial mask")
}

fn luminance_mask() -> MaskDefinition {
    MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.1), false).expect("luminance mask")
}

fn semantic_mask(
    query: &str,
    expansion_percent: i8,
    feather_percent: u8,
    invert: bool,
    seed: char,
) -> MaskDefinition {
    let (raster, _, _) = managed_raster(seed);
    MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
        raster,
        Some(intent(query)),
        expansion_percent,
        feather_percent,
        invert,
    )
    .expect("semantic mask")
}

fn nonsemantic_mask(seed: char) -> MaskDefinition {
    let (raster, _, _) = managed_raster(seed);
    MaskDefinition::managed_raster(raster, false).expect("managed mask")
}

fn target_semantic_mask(item: &super::PendingSemanticLeaf, seed: char) -> MaskDefinition {
    let (raster, _, _) = managed_raster(seed);
    MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
        raster,
        Some(item.intent.clone()),
        item.expansion_percent,
        item.feather_percent,
        item.leaf_invert,
    )
    .expect("target semantic mask")
}

fn intent(query: &str) -> SemanticMaskIntent {
    SemanticMaskIntent::new(query, 4, 30, SemanticMaskAggregation::Union).expect("semantic intent")
}

fn managed_raster(seed: char) -> (ManagedRasterMask, String, String) {
    let content_hash = seed.to_string().repeat(64);
    let store_object_id = format!(
        "objects/v1/b3/{}/{}",
        &content_hash[..2],
        &content_hash[2..]
    );
    let raster = ManagedRasterMask::new(
        store_object_id.clone(),
        1,
        content_hash.clone(),
        16,
        4,
        4,
        400,
        300,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    (raster, store_object_id, content_hash)
}

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("unit interval")
}

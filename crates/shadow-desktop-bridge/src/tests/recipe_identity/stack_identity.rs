//! Grade Stack ordering, identity preservation, and capacity contracts.

use shadow_bridge::AdjustmentRenderOperation;
use shadow_domain::{
    CURRENT_RECIPE_SCHEMA_VERSION, EntityId, NodeId, PhotoGeometry, RecipeOpticsSettings,
    RecipeSnapshot, diff_recipe_snapshots,
};

use crate::recipe_v1::{
    GradeNodeDraft, GradeStackDraft, MAX_GRADE_NODES, compile_recipe_render_plan,
    decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
    encode_grade_node_as_recipe_v1_layer, encode_grade_stack_draft_recipe_v1,
    grade_stack_recipe_v1_snapshot, new_basic_grade_node,
};

#[test]
fn two_grade_nodes_compile_in_recipe_vector_order_with_namespaced_render_ops() {
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.basic.exposure_stops = 0.5;
    let mut second = GradeNodeDraft::neutral("Second Basic");
    second.basic.exposure_stops = -1.25;
    grade_stack.grade_nodes.push(second);
    let first_grade_node_id = grade_stack.grade_nodes[0].recipe_v1_identity.grade_node_id;
    let second_grade_node_id = grade_stack.grade_nodes[1].recipe_v1_identity.grade_node_id;

    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("two-node snapshot");
    let plan = compile_recipe_render_plan(&snapshot).expect("compile two Grade Nodes");
    assert_eq!(plan.nodes.len(), 20);
    assert!(
        plan.nodes[..10]
            .iter()
            .all(|node| node.node_id.starts_with(&format!("{first_grade_node_id}/")))
    );
    assert!(plan.nodes[10..].iter().all(|node| {
        node.node_id
            .starts_with(&format!("{second_grade_node_id}/"))
    }));
    assert!(matches!(
        plan.nodes[1].operation,
        AdjustmentRenderOperation::Exposure { stops: 0.5 }
    ));
    assert!(matches!(
        plan.nodes[11].operation,
        AdjustmentRenderOperation::Exposure { stops: -1.25 }
    ));

    grade_stack.grade_nodes.reverse();
    let reversed = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("reordered snapshot");
    let reversed_plan = compile_recipe_render_plan(&reversed).expect("compile reordered stack");
    assert!(
        reversed_plan.nodes[0]
            .node_id
            .starts_with(&format!("{second_grade_node_id}/"))
    );
}

#[test]
fn grade_stack_rejects_cross_grade_node_render_op_identity_reuse() {
    let first = GradeNodeDraft::neutral("First Basic");
    let mut second = GradeNodeDraft::neutral("Second Basic");
    second.recipe_v1_identity.exposure_render_op_id =
        first.recipe_v1_identity.exposure_render_op_id;
    let invalid = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![first.clone(), second.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };

    let ffi_error = decode_grade_stack_draft_recipe_v1(
        &encode_grade_stack_draft_recipe_v1(invalid.clone()).expect("encode invalid draft"),
    )
    .expect_err("FFI stack must reject a render-op id reused by another Grade Node");
    assert!(
        ffi_error
            .to_string()
            .contains("duplicate Recipe v1 render-op id")
    );

    // RecipeSnapshot currently scopes graph identity validation per layer,
    // so the desktop compiler must independently enforce the stack-wide
    // identity contract for externally persisted snapshots.
    let persisted = RecipeSnapshot::new(
        CURRENT_RECIPE_SCHEMA_VERSION,
        vec![
            encode_grade_node_as_recipe_v1_layer(&first).expect("first persisted Grade Node"),
            encode_grade_node_as_recipe_v1_layer(&second).expect("second persisted Grade Node"),
        ],
    )
    .expect("domain-valid graph-scoped identities");
    assert!(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&persisted)
            .expect_err("persisted stack read must reject reused render-op identity")
            .to_string()
            .contains("duplicate Recipe v1 render-op id")
    );
    assert!(
        compile_recipe_render_plan(&persisted)
            .expect_err("compiler must reject reused render-op identity")
            .to_string()
            .contains("duplicate render-op id")
    );
}

#[test]
fn duplicate_add_delete_and_reorder_preserve_the_expected_identities() {
    let mut original = GradeStackDraft::default();
    original.fine.selective_tone.shadows = 0.2;
    original.fine.perceptual_color.vibrance = 0.35;
    let base = grade_stack_recipe_v1_snapshot(&original, None).expect("base snapshot");
    let duplicate = original.grade_nodes[0].duplicate();
    assert_eq!(duplicate.basic, original.grade_nodes[0].basic);
    assert_eq!(duplicate.fine, original.grade_nodes[0].fine);
    assert_ne!(
        duplicate.recipe_v1_identity.grade_node_id,
        original.grade_nodes[0].recipe_v1_identity.grade_node_id
    );
    assert!(
        duplicate
            .recipe_v1_identity
            .recipe_v1_render_op_id_values()
            .into_iter()
            .all(|id| !original.grade_nodes[0]
                .recipe_v1_identity
                .recipe_v1_render_op_id_values()
                .contains(&id))
    );

    let mut added_settings = original.clone();
    added_settings.grade_nodes.push(duplicate.clone());
    let added =
        grade_stack_recipe_v1_snapshot(&added_settings, Some(&base)).expect("added snapshot");
    let added_diff = diff_recipe_snapshots(&base, &added);
    assert_eq!(added_diff.added_layers().len(), 1);
    assert_eq!(
        added_diff.added_layers()[0].id(),
        duplicate.recipe_v1_identity.grade_node_id
    );

    let mut reordered_settings = added_settings.clone();
    reordered_settings.grade_nodes.swap(0, 1);
    let reordered = grade_stack_recipe_v1_snapshot(&reordered_settings, Some(&added))
        .expect("reordered snapshot");
    let reordered_diff = diff_recipe_snapshots(&added, &reordered);
    assert_eq!(reordered_diff.moved_layers().len(), 2);
    assert_eq!(
        reordered.layers()[0].id(),
        duplicate.recipe_v1_identity.grade_node_id
    );

    reordered_settings.grade_nodes.remove(0);
    let deleted = grade_stack_recipe_v1_snapshot(&reordered_settings, Some(&reordered))
        .expect("deleted snapshot");
    assert_eq!(deleted, base);
    assert_eq!(
        diff_recipe_snapshots(&reordered, &deleted)
            .removed_layers()
            .len(),
        1
    );
}

#[test]
fn template_rejects_retained_identity_rewrite_and_deleted_node_reuse() {
    let mut base_settings = GradeStackDraft::default();
    base_settings
        .grade_nodes
        .push(GradeNodeDraft::neutral("Second Basic"));
    let base = grade_stack_recipe_v1_snapshot(&base_settings, None).expect("two-node base");

    let mut rewritten = base_settings.clone();
    rewritten.grade_nodes[0]
        .recipe_v1_identity
        .exposure_render_op_id = NodeId::new_v7();
    assert!(
        grade_stack_recipe_v1_snapshot(&rewritten, Some(&base))
            .expect_err("retained Grade Node render-op identity rewrite must fail")
            .to_string()
            .contains("must preserve every stable Recipe v1 render-op identity")
    );

    let deleted_exposure_id = base_settings.grade_nodes[0]
        .recipe_v1_identity
        .exposure_render_op_id;
    let mut replacement = GradeNodeDraft::neutral("Replacement Basic");
    replacement.recipe_v1_identity.exposure_render_op_id = deleted_exposure_id;
    let replacement_settings = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![base_settings.grade_nodes[1].clone(), replacement],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };
    assert!(
        grade_stack_recipe_v1_snapshot(&replacement_settings, Some(&base))
            .expect_err("new Grade Node must not reuse a deleted base render-op identity")
            .to_string()
            .contains("reuses base Recipe v1 render-op id")
    );
}

#[test]
fn grade_stack_accepts_sixteen_grade_nodes_and_rejects_seventeen() {
    assert!(
        grade_stack_recipe_v1_snapshot(
            &GradeStackDraft {
                optics: RecipeOpticsSettings::default(),
                grade_nodes: Vec::new(),
                retouch_spots: Vec::new(),
                retouch_strokes: Vec::new(),
                geometry: PhotoGeometry::identity(),
            },
            None
        )
        .expect_err("an empty stack must fail closed")
        .to_string()
        .contains("1 through 16")
    );
    let sixteen = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: (0..MAX_GRADE_NODES)
            .map(|index| GradeNodeDraft::neutral(format!("Basic {index}")))
            .collect(),
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };
    let snapshot = grade_stack_recipe_v1_snapshot(&sixteen, None).expect("sixteen-node snapshot");
    assert_eq!(
        compile_recipe_render_plan(&snapshot).unwrap().nodes.len(),
        160
    );

    let mut seventeen = sixteen.clone();
    seventeen
        .grade_nodes
        .push(GradeNodeDraft::neutral("One too many"));
    let error = grade_stack_recipe_v1_snapshot(&seventeen, None)
        .expect_err("seventeen Grade Nodes must fail closed");
    assert!(error.to_string().contains("1 through 16"));

    let mut ffi_seventeen =
        encode_grade_stack_draft_recipe_v1(sixteen).expect("encode sixteen nodes");
    ffi_seventeen
        .grade_nodes
        .push(new_basic_grade_node("One too many").unwrap());
    assert!(
        decode_grade_stack_draft_recipe_v1(&ffi_seventeen)
            .expect_err("FFI seventeen Grade Nodes must fail")
            .to_string()
            .contains("1 through 16")
    );
}

//! Photo-local geometry persistence and render-plan contracts.

use shadow_bridge::{AdjustmentLiquifyStroke, AdjustmentQuarterTurn};
use shadow_domain::{
    CURRENT_RECIPE_SCHEMA_VERSION, LiquifyPoint, LiquifyStroke, PhotoCanvasNode, PhotoGeometry,
    PhotoLiquifyNode, PhotoQuarterTurn, PhotoStructuralNodes, RecipeSnapshot, UnitInterval,
};

use crate::recipe_v1::{
    GradeStackDraft, compile_recipe_render_plan, decode_grade_stack_draft_from_recipe_v1_snapshot,
    decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1,
    grade_stack_recipe_v1_snapshot,
};

#[test]
#[allow(clippy::float_cmp)] // This contract requires bit-exact persisted geometry.
fn photo_geometry_round_trips_without_becoming_a_grade_node() {
    let geometry = PhotoGeometry::new(
        UnitInterval::new(0.125).expect("crop left"),
        UnitInterval::new(0.25).expect("crop top"),
        UnitInterval::new(0.875).expect("crop right"),
        UnitInterval::new(0.75).expect("crop bottom"),
        PhotoQuarterTurn::Clockwise90,
        true,
        false,
    )
    .expect("valid photo-local geometry");
    let grade_stack = GradeStackDraft {
        canvas: PhotoCanvasNode::new(geometry),
        ..GradeStackDraft::default()
    };

    let snapshot =
        grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist photo geometry");
    assert_eq!(snapshot.canvas_node().geometry(), geometry);
    assert_eq!(snapshot.geometry(), geometry);
    assert_eq!(snapshot.layers().len(), 1, "geometry is not a Grade Node");

    let plan = compile_recipe_render_plan(&snapshot).expect("compile photo geometry");
    assert_eq!(plan.geometry.crop_left, 0.125);
    assert_eq!(plan.geometry.crop_top, 0.25);
    assert_eq!(plan.geometry.crop_right, 0.875);
    assert_eq!(plan.geometry.crop_bottom, 0.75);
    assert_eq!(
        plan.geometry.quarter_turn,
        AdjustmentQuarterTurn::Clockwise90
    );
    assert!(plan.geometry.flip_horizontal);
    assert!(!plan.geometry.flip_vertical);

    let ffi = encode_grade_stack_draft_recipe_v1(grade_stack).expect("encode Grade Stack");
    assert!(ffi.geometry.present);
    assert!(ffi.geometry.enabled);
    assert_eq!(ffi.geometry.quarter_turn, 1);
    assert!(ffi.geometry.flip_horizontal);
    assert!(!ffi.geometry.flip_vertical);
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode photo geometry");
    assert_eq!(decoded.canvas.geometry(), geometry);
}

#[test]
#[allow(clippy::float_cmp)] // The FFI field is the exact authored Recipe value, not a computed result.
fn bypassed_canvas_round_trips_authored_geometry_without_compiling_it() {
    let geometry = PhotoGeometry::identity()
        .with_straighten_degrees(3.0)
        .expect("valid authored straighten");
    let grade_stack = GradeStackDraft {
        canvas: PhotoCanvasNode::new(geometry).with_enabled(false),
        ..GradeStackDraft::default()
    };

    let ffi = encode_grade_stack_draft_recipe_v1(grade_stack).expect("encode bypassed Canvas");
    assert!(ffi.geometry.present);
    assert!(!ffi.geometry.enabled);
    assert_eq!(ffi.geometry.straighten_degrees, 3.0);

    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode bypassed Canvas");
    assert!(decoded.canvas.is_present());
    assert!(!decoded.canvas.enabled());
    assert_eq!(decoded.canvas.geometry(), geometry);

    let snapshot = grade_stack_recipe_v1_snapshot(&decoded, None).expect("persist bypassed Canvas");
    assert_eq!(snapshot.canvas_node().geometry(), geometry);
    assert_eq!(snapshot.geometry(), PhotoGeometry::identity());
    let plan = compile_recipe_render_plan(&snapshot).expect("compile bypassed Canvas");
    assert_eq!(plan.geometry, shadow_bridge::AdjustmentGeometry::identity());
}

#[test]
#[allow(clippy::float_cmp)] // The compiled normalized wire is exact Recipe identity.
fn crop_edits_preserve_liquify_and_the_compiler_projects_exact_warp_values() {
    let base =
        grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None).expect("base Recipe");
    let point = |x, y| {
        LiquifyPoint::new(
            UnitInterval::new(x).expect("normalized x"),
            UnitInterval::new(y).expect("normalized y"),
        )
    };
    let liquify = PhotoLiquifyNode::new(vec![
        LiquifyStroke::push(
            vec![point(0.3, 0.4), point(0.35, 0.45)],
            UnitInterval::new(0.1).expect("radius"),
            UnitInterval::new(0.75).expect("strength"),
            UnitInterval::new(0.5).expect("hardness"),
        )
        .expect("push stroke"),
    ])
    .expect("liquify node");
    let structural =
        RecipeSnapshot::new_with_input_settings_masks_retouch_strokes_and_structural_nodes(
            CURRENT_RECIPE_SCHEMA_VERSION,
            base.input_settings().clone(),
            base.masks().to_vec(),
            base.retouch_spots().to_vec(),
            base.retouch_strokes().to_vec(),
            PhotoStructuralNodes::new(Some(liquify.clone()), PhotoCanvasNode::identity())
                .expect("structural nodes"),
            base.layers().to_vec(),
        )
        .expect("Recipe with Liquify");

    let plan = compile_recipe_render_plan(&structural).expect("compile Liquify render plan");
    let compiled = plan.liquify.as_ref().expect("compiled Liquify singleton");
    let [stroke] = compiled.strokes.as_slice() else {
        panic!("expected one compiled Liquify push stroke")
    };
    let AdjustmentLiquifyStroke::Push(stroke) = stroke else {
        panic!("expected compiled Push operation")
    };
    assert_eq!(stroke.radius, 0.1);
    assert_eq!(stroke.strength, 0.75);
    assert_eq!(stroke.hardness, 0.5);
    let [first, second] = stroke.points.as_slice() else {
        panic!("expected two compiled Liquify pointer samples")
    };
    assert_eq!((first.x, first.y, first.pressure), (0.3, 0.4, 1.0));
    assert_eq!((second.x, second.y, second.pressure), (0.35, 0.45, 1.0));

    let mut crop_projection = decode_grade_stack_draft_from_recipe_v1_snapshot(&structural)
        .expect("decode Canvas projection");
    let changed_geometry = PhotoGeometry::new(
        UnitInterval::new(0.1).expect("crop left"),
        UnitInterval::new(0.2).expect("crop top"),
        UnitInterval::new(0.9).expect("crop right"),
        UnitInterval::new(0.8).expect("crop bottom"),
        PhotoQuarterTurn::Zero,
        false,
        false,
    )
    .expect("changed crop");
    crop_projection.canvas = PhotoCanvasNode::new(changed_geometry);
    let rebuilt = grade_stack_recipe_v1_snapshot(&crop_projection, Some(&structural))
        .expect("rebuild from Canvas projection");

    assert_eq!(rebuilt.canvas_node().geometry(), changed_geometry);
    assert_eq!(
        rebuilt.structural_nodes().liquify(),
        Some(&liquify),
        "an unrelated Canvas edit must preserve the exact Liquify node"
    );

    crop_projection.liquify = None;
    let cleared = grade_stack_recipe_v1_snapshot(&crop_projection, Some(&structural))
        .expect("remove Liquify through the editable projection");
    assert!(
        cleared.structural_nodes().liquify().is_none(),
        "an explicit empty desktop projection removes the optional singleton"
    );
}

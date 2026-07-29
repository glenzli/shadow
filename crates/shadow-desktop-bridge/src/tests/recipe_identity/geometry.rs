//! Photo-local geometry persistence and render-plan contracts.

use shadow_bridge::AdjustmentQuarterTurn;
use shadow_domain::{PhotoGeometry, PhotoQuarterTurn, UnitInterval};

use crate::recipe_v1::{
    GradeStackDraft, compile_recipe_render_plan, decode_grade_stack_draft_recipe_v1,
    encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
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
        geometry,
        ..GradeStackDraft::default()
    };

    let snapshot =
        grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist photo geometry");
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
    assert_eq!(ffi.geometry.quarter_turn, 1);
    assert!(ffi.geometry.flip_horizontal);
    assert!(!ffi.geometry.flip_vertical);
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode photo geometry");
    assert_eq!(decoded.geometry, geometry);
}

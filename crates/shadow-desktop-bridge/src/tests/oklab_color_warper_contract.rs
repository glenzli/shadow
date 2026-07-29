//! Desktop DTO, persistence, and render-plan contracts for Oklab Color Warper.

use shadow_bridge::{
    AdjustmentRenderOperation, OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT, OklabColorWarperControlPoint,
};

use crate::{
    recipe_v1::{
        GradeStackDraft, compile_recipe_render_plan, decode_grade_stack_draft_recipe_v1,
        encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
        single_grade_node_recipe_v1_render_ops,
    },
    tests::fixtures::grade_stack::ffi_parameters,
};

#[test]
#[allow(clippy::float_cmp)] // The fixed lattice projection is an exact persistence contract.
fn oklab_color_warper_elides_neutral_lattice_and_preserves_fixed_mapping() {
    let neutral = GradeStackDraft::default();
    let neutral_snapshot =
        grade_stack_recipe_v1_snapshot(&neutral, None).expect("persist neutral Grade Node");
    assert!(
        single_grade_node_recipe_v1_render_ops(&neutral_snapshot)
            .expect("read neutral Grade Node")
            .oklab_color_warper
            .is_none()
    );

    let mut authored = neutral;
    authored.fine.oklab_color_warper.control_points[0] = OklabColorWarperControlPoint {
        a_offset: -0.12,
        b_offset: 0.08,
    };
    authored.fine.oklab_color_warper.control_points[OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT - 1] =
        OklabColorWarperControlPoint {
            a_offset: 0.11,
            b_offset: -0.09,
        };
    authored.fine.oklab_color_warper.strength = 0.63;
    let identity = authored.recipe_v1_identity.oklab_color_warper_render_op_id;

    let ffi = encode_grade_stack_draft_recipe_v1(authored.clone()).expect("encode Grade Stack");
    assert_eq!(
        ffi.grade_nodes[0]
            .fine
            .oklab_color_warper_control_points
            .len(),
        OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2
    );
    assert_eq!(
        ffi.grade_nodes[0].fine.oklab_color_warper_control_points[0..4],
        [-0.12, 0.08, 0.0, 0.0]
    );
    assert_eq!(
        ffi.grade_nodes[0].fine.oklab_color_warper_control_points[48..],
        [0.11, -0.09]
    );
    assert_eq!(ffi.grade_nodes[0].fine.oklab_color_warper_strength, 0.63);
    assert_eq!(
        decode_grade_stack_draft_recipe_v1(&ffi)
            .expect("decode Color Warper desktop DTO")
            .fine
            .oklab_color_warper,
        authored.fine.oklab_color_warper
    );

    let snapshot = grade_stack_recipe_v1_snapshot(&authored, Some(&neutral_snapshot))
        .expect("persist authored Color Warper");
    let recipe_nodes =
        single_grade_node_recipe_v1_render_ops(&snapshot).expect("read authored Grade Node");
    assert_eq!(
        recipe_nodes
            .oklab_color_warper
            .expect("Color Warper render operation")
            .id(),
        identity
    );
    let plan = compile_recipe_render_plan(&snapshot).expect("compile Color Warper");
    assert!(matches!(
        &plan.nodes[6].operation,
        AdjustmentRenderOperation::OklabColorWarper { parameters }
            if parameters.as_ref() == &authored.fine.oklab_color_warper
    ));

    let mut malformed = ffi;
    malformed.grade_nodes[0]
        .fine
        .oklab_color_warper_control_points
        .pop();
    assert!(
        decode_grade_stack_draft_recipe_v1(&malformed)
            .expect_err("a Color Warper DTO must contain exactly 25 a/b pairs")
            .to_string()
            .contains("oklab_color_warper_control_points")
    );
}

#[test]
fn oklab_color_warper_ffi_validation_rejects_noncanonical_controls() {
    let mut invalid_strength = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_strength.fine.oklab_color_warper_strength = 1.01;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_strength)
            .expect_err("Color Warper strength must stay normalized")
            .to_string()
            .contains("Oklab Color Warper strength")
    );

    let mut invalid_offset = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_offset.fine.oklab_color_warper_control_points[0] =
        shadow_bridge::OKLAB_COLOR_WARPER_MAXIMUM_OFFSET + 0.001;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_offset)
            .expect_err("Color Warper offsets must stay within the lattice contract")
            .to_string()
            .contains("Oklab Color Warper a offset")
    );
}

use shadow_bridge::{OklabLightnessToneCurve, ToneCurvePoint};

use super::*;
use crate::recipe_v1::{GradeStackDraft, grade_stack_recipe_v1_snapshot};

#[test]
fn layout_projects_neutral_and_optional_operation_slots_in_canonical_order() {
    let neutral = GradeStackDraft::default();
    let neutral_snapshot =
        grade_stack_recipe_v1_snapshot(&neutral, None).expect("encode neutral Grade Node");
    let neutral_nodes =
        single_grade_node_recipe_v1_render_ops(&neutral_snapshot).expect("project neutral layout");
    assert!(neutral_nodes.oklab_color_warper.is_none());
    assert!(neutral_nodes.oklab_lightness_curve.is_none());
    assert_eq!(neutral_nodes.ordered().len(), 10);

    let mut authored = GradeStackDraft::default();
    authored.fine.oklab_color_warper.control_points[0].a_offset = 0.05;
    authored.fine.oklab_lightness_curve = Some(OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.5, y: 0.65 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    });
    let expected = &authored.recipe_v1_identity;
    let expected_ids = [
        expected.white_balance_render_op_id,
        expected.exposure_render_op_id,
        expected.contrast_render_op_id,
        expected.selective_tone_render_op_id,
        expected.saturation_render_op_id,
        expected.perceptual_color_render_op_id,
        expected.oklab_color_warper_render_op_id,
        expected.oklab_lightness_curve_render_op_id,
        expected.sharpen_render_op_id,
        expected.color_grading_render_op_id,
        expected.lut_render_op_id,
        expected.finishing_effects_render_op_id,
    ];
    let authored_snapshot =
        grade_stack_recipe_v1_snapshot(&authored, None).expect("encode authored Grade Node");
    let authored_nodes = single_grade_node_recipe_v1_render_ops(&authored_snapshot)
        .expect("project authored layout");
    assert_eq!(
        authored_nodes
            .ordered()
            .into_iter()
            .map(AdjustmentNode::id)
            .collect::<Vec<_>>(),
        expected_ids
    );
}

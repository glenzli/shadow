use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, MaskDefinition,
    UnitInterval,
};

use super::super::{
    GradeStackDraft, decode_grade_stack_draft_from_recipe_v1_snapshot,
    grade_stack_recipe_v1_snapshot,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

#[test]
fn persisted_but_unexecutable_conditions_fail_before_qt_projection() {
    let expression = ConditionMaskExpression::all(vec![
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
            unit(0.2),
            unit(0.8),
            unit(0.1),
        )),
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklch_chroma_range(
            unit(0.25),
            unit(0.9),
            unit(0.15),
        )),
    ])
    .expect("expression");
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.grade_nodes[0].local_mask =
        Some(MaskDefinition::condition_expression(expression).expect("condition mask"));
    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persistable Recipe");

    let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect_err("current desktop DTO must reject unsupported condition");
    assert!(
        error
            .to_string()
            .contains("current editable Grade Stack cannot project")
    );
}

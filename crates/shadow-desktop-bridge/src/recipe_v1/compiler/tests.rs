use shadow_bridge::AdjustmentLocalMask;
use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, MaskDefinition,
    UnitInterval,
};

use super::adjustment_local_mask;

#[test]
fn condition_masks_compile_without_changing_authored_units() {
    let luminance = MaskDefinition::luminance_range(
        UnitInterval::new(0.2).expect("lower"),
        UnitInterval::new(0.8).expect("upper"),
        UnitInterval::new(0.15).expect("softness"),
        true,
    )
    .expect("luminance range");
    assert_eq!(
        adjustment_local_mask(&luminance).expect("compile luminance"),
        AdjustmentLocalMask::LuminanceRange {
            lower: 0.2,
            upper: 0.8,
            softness: 0.15,
            invert: true,
        }
    );

    let color = MaskDefinition::color_range(
        359.5,
        72.0,
        UnitInterval::new(0.4).expect("softness"),
        false,
    )
    .expect("color range");
    assert_eq!(
        adjustment_local_mask(&color).expect("compile color"),
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: 359.5,
            width_degrees: 72.0,
            softness: 0.4,
            invert: false,
        }
    );
}

#[test]
fn composite_condition_masks_fail_at_the_executability_boundary() {
    let unit = |value| UnitInterval::new(value).expect("unit interval");
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
    .expect("composite expression");
    let definition =
        MaskDefinition::condition_expression(expression).expect("persistent condition mask");
    let error = adjustment_local_mask(&definition).expect_err("runtime must reject unsupported");
    assert!(
        error
            .to_string()
            .contains("persists bounded condition-mask expressions")
    );
}

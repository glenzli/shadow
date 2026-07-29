use super::*;
use crate::recipe::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, FiniteF64,
    RecipeValidationError, UnitInterval,
};

#[test]
fn local_masks_reject_degenerate_geometry() {
    assert_eq!(
        MaskDefinition::linear_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        ),
        Err(RecipeValidationError::DegenerateLinearMask)
    );
    assert_eq!(
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::ZERO,
            UnitInterval::new(0.3).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        ),
        Err(RecipeValidationError::DegenerateRadialMask)
    );
}

#[test]
fn brush_masks_round_trip_multiple_editable_strokes() {
    let definition = MaskDefinition::brush(
        vec![
            MaskBrushPoint::new(
                UnitInterval::new(0.2).expect("x"),
                UnitInterval::new(0.3).expect("y"),
                true,
            ),
            MaskBrushPoint::new(
                UnitInterval::new(0.4).expect("x"),
                UnitInterval::new(0.5).expect("y"),
                false,
            ),
            MaskBrushPoint::new(
                UnitInterval::new(0.7).expect("x"),
                UnitInterval::new(0.6).expect("y"),
                true,
            ),
        ],
        UnitInterval::new(0.04).expect("radius"),
        UnitInterval::new(0.6).expect("feather"),
        false,
    )
    .expect("valid brush");
    let encoded = serde_json::to_string(&definition).expect("serialize brush");
    assert!(encoded.contains("\"kind\":\"brush\""));
    let decoded: MaskDefinition = serde_json::from_str(&encoded).expect("deserialize brush");
    assert_eq!(decoded, definition);

    assert_eq!(
        MaskDefinition::brush(
            Vec::new(),
            UnitInterval::ZERO,
            UnitInterval::new(0.5).expect("feather"),
            false,
        ),
        Err(RecipeValidationError::DegenerateBrushMask)
    );
}

#[test]
fn legacy_mask_json_contract_is_byte_stable() {
    let linear = MaskDefinition::linear_gradient(
        UnitInterval::new(0.1).expect("start x"),
        UnitInterval::new(0.2).expect("start y"),
        UnitInterval::new(0.8).expect("end x"),
        UnitInterval::new(0.9).expect("end y"),
        true,
    )
    .expect("linear mask");
    assert_eq!(
        serde_json::to_string(&linear).expect("serialize linear mask"),
        r#"{"kind":"linear_gradient","start_x":0.1,"start_y":0.2,"end_x":0.8,"end_y":0.9,"invert":true}"#
    );

    let radial = MaskDefinition::radial_gradient(
        UnitInterval::new(0.4).expect("center x"),
        UnitInterval::new(0.6).expect("center y"),
        UnitInterval::new(0.2).expect("radius x"),
        UnitInterval::new(0.3).expect("radius y"),
        UnitInterval::new(0.5).expect("feather"),
        false,
    )
    .expect("radial mask");
    assert_eq!(
        serde_json::to_string(&radial).expect("serialize radial mask"),
        r#"{"kind":"radial_gradient","center_x":0.4,"center_y":0.6,"radius_x":0.2,"radius_y":0.3,"feather":0.5,"invert":false}"#
    );

    let brush = MaskDefinition::brush(
        vec![MaskBrushPoint::new(
            UnitInterval::new(0.25).expect("x"),
            UnitInterval::new(0.75).expect("y"),
            true,
        )],
        UnitInterval::new(0.04).expect("radius"),
        UnitInterval::new(0.6).expect("feather"),
        true,
    )
    .expect("brush mask");
    assert_eq!(
        serde_json::to_string(&brush).expect("serialize brush mask"),
        r#"{"kind":"brush","points":[{"x":0.25,"y":0.75,"begins_stroke":true}],"radius":0.04,"feather":0.6,"invert":true}"#
    );
}

#[test]
fn luminance_range_requires_ordered_bounds_and_round_trips() {
    let definition = MaskDefinition::luminance_range(
        UnitInterval::new(0.2).expect("lower"),
        UnitInterval::new(0.8).expect("upper"),
        UnitInterval::new(0.15).expect("softness"),
        true,
    )
    .expect("luminance range");
    let encoded = serde_json::to_string(&definition).expect("serialize luminance range");
    assert_eq!(
        encoded,
        r#"{"kind":"luminance_range","lower":0.2,"upper":0.8,"softness":0.15,"invert":true}"#
    );
    assert_eq!(
        serde_json::from_str::<MaskDefinition>(&encoded).expect("deserialize luminance range"),
        definition
    );

    assert_eq!(
        MaskDefinition::luminance_range(
            UnitInterval::new(0.8).expect("lower"),
            UnitInterval::new(0.2).expect("upper"),
            UnitInterval::new(0.15).expect("softness"),
            false,
        ),
        Err(RecipeValidationError::InvalidLuminanceMaskRange {
            lower: 0.8,
            upper: 0.2,
        })
    );
}

#[test]
#[allow(clippy::float_cmp)] // Normalization and persisted scalar projection are exact contracts.
fn color_range_normalizes_hue_and_rejects_invalid_width() {
    let definition = MaskDefinition::color_range(
        725.0,
        35.0,
        UnitInterval::new(0.4).expect("softness"),
        false,
    )
    .expect("color range");
    let MaskDefinition::ColorRange {
        center_hue_degrees,
        width_degrees,
        softness,
        invert,
    } = &definition
    else {
        panic!("expected color range")
    };
    assert_eq!(center_hue_degrees.get(), 5.0);
    assert_eq!(width_degrees.get(), 35.0);
    assert_eq!(softness.get(), 0.4);
    assert!(!invert);

    let encoded = serde_json::to_string(&definition).expect("serialize color range");
    assert_eq!(
        encoded,
        r#"{"kind":"color_range","center_hue_degrees":5.0,"width_degrees":35.0,"softness":0.4,"invert":false}"#
    );
    assert_eq!(
        serde_json::from_str::<MaskDefinition>(&encoded).expect("deserialize color range"),
        definition
    );

    let wrapped_negative = MaskDefinition::color_range(-10.0, 180.0, UnitInterval::ZERO, false)
        .expect("negative hue normalizes");
    let MaskDefinition::ColorRange {
        center_hue_degrees, ..
    } = wrapped_negative
    else {
        panic!("expected color range")
    };
    assert_eq!(center_hue_degrees.get(), 350.0);

    for width in [0.0, 180.01] {
        assert_eq!(
            MaskDefinition::color_range(20.0, width, UnitInterval::ZERO, false),
            Err(RecipeValidationError::InvalidColorMaskWidth(width))
        );
    }
    assert_eq!(
        MaskDefinition::color_range(f64::NAN, 30.0, UnitInterval::ZERO, false),
        Err(RecipeValidationError::NonFiniteNumber)
    );

    let noncanonical = MaskDefinition::ColorRange {
        center_hue_degrees: FiniteF64::new(360.0).expect("finite hue"),
        width_degrees: FiniteF64::new(30.0).expect("finite width"),
        softness: UnitInterval::ZERO,
        invert: false,
    };
    assert_eq!(
        noncanonical.validate(),
        Err(RecipeValidationError::InvalidColorMaskHue(360.0))
    );
}

#[test]
fn single_condition_leaves_keep_the_existing_persistent_mask_shapes() {
    let luminance = ConditionMaskExpression::leaf(ConditionMaskPredicate::oklab_lightness_range(
        UnitInterval::new(0.2).expect("lower"),
        UnitInterval::new(0.8).expect("upper"),
        UnitInterval::new(0.15).expect("softness"),
    ))
    .expect("luminance expression");
    let luminance =
        MaskDefinition::condition_expression(luminance).expect("canonical luminance mask");
    assert_eq!(
        serde_json::to_string(&luminance).expect("serialize luminance"),
        r#"{"kind":"luminance_range","lower":0.2,"upper":0.8,"softness":0.15,"invert":false}"#
    );

    let inverted_hue = ConditionMaskExpression::not(ConditionMaskNode::leaf(
        ConditionMaskPredicate::oklch_hue_range(
            725.0,
            35.0,
            UnitInterval::ZERO,
            UnitInterval::new(0.4).expect("softness"),
        )
        .expect("hue predicate"),
    ))
    .expect("inverted hue expression");
    let inverted_hue =
        MaskDefinition::condition_expression(inverted_hue).expect("canonical hue mask");
    assert_eq!(
        serde_json::to_string(&inverted_hue).expect("serialize hue"),
        r#"{"kind":"color_range","center_hue_degrees":5.0,"width_degrees":35.0,"softness":0.4,"invert":true}"#
    );
}

#[test]
fn composite_conditions_persist_but_reducible_direct_variants_are_rejected() {
    let expression = ConditionMaskExpression::all(vec![
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
            UnitInterval::new(0.2).expect("lower"),
            UnitInterval::new(0.8).expect("upper"),
            UnitInterval::new(0.1).expect("softness"),
        )),
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklch_chroma_range(
            UnitInterval::new(0.25).expect("lower"),
            UnitInterval::new(0.9).expect("upper"),
            UnitInterval::new(0.15).expect("softness"),
        )),
    ])
    .expect("composite");
    let definition =
        MaskDefinition::condition_expression(expression.clone()).expect("condition mask");
    assert!(matches!(
        definition,
        MaskDefinition::ConditionExpression { .. }
    ));
    let encoded = serde_json::to_string(&definition).expect("serialize condition mask");
    assert_eq!(
        serde_json::from_str::<MaskDefinition>(&encoded).expect("deserialize condition mask"),
        definition
    );

    let reducible = ConditionMaskExpression::leaf(ConditionMaskPredicate::oklab_lightness_range(
        UnitInterval::new(0.2).expect("lower"),
        UnitInterval::new(0.8).expect("upper"),
        UnitInterval::new(0.1).expect("softness"),
    ))
    .expect("single leaf");
    assert_eq!(
        MaskDefinition::ConditionExpression {
            expression: reducible
        }
        .validate(),
        Err(RecipeValidationError::NonCanonicalConditionMaskExpression)
    );
}

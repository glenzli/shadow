use super::*;
use crate::recipe::{RecipeValidationError, UnitInterval};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

fn lightness(lower: f64, upper: f64) -> ConditionMaskNode {
    ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
        unit(lower),
        unit(upper),
        unit(0.1),
    ))
}

#[test]
fn expression_round_trip_has_a_stable_typed_payload() {
    let expression = ConditionMaskExpression::all(vec![
        lightness(0.2, 0.8),
        ConditionMaskNode::negated(ConditionMaskNode::leaf(
            ConditionMaskPredicate::oklch_hue_range(725.0, 35.0, unit(0.2), unit(0.15))
                .expect("hue range"),
        )),
    ])
    .expect("bounded expression");
    let encoded = serde_json::to_string(&expression).expect("serialize expression");
    assert_eq!(
        encoded,
        r#"{"schema_version":1,"root":{"operator":"all","children":[{"operator":"leaf","condition":{"kind":"oklab_lightness_range","lower":0.2,"upper":0.8,"softness":0.1}},{"operator":"not","child":{"operator":"leaf","condition":{"kind":"oklch_hue_range","center_hue_degrees":5.0,"half_width_degrees":35.0,"minimum_chroma":0.2,"minimum_chroma_feather":0.0,"softness":0.15}}}]}}"#
    );
    assert_eq!(
        serde_json::from_str::<ConditionMaskExpression>(&encoded).expect("deserialize expression"),
        expression
    );
}

#[test]
fn hue_payloads_without_the_new_absolute_chroma_feather_remain_readable() {
    let encoded = r#"{"schema_version":1,"root":{"operator":"leaf","condition":{"kind":"oklch_hue_range","center_hue_degrees":5.0,"half_width_degrees":35.0,"minimum_chroma":0.2,"softness":0.15}}}"#;
    let expression =
        serde_json::from_str::<ConditionMaskExpression>(encoded).expect("legacy v1 hue payload");
    let ConditionMaskNode::Leaf {
        condition:
            ConditionMaskPredicate::OklchHueRange {
                minimum_chroma_feather,
                ..
            },
    } = expression.root()
    else {
        panic!("expected hue leaf");
    };
    assert_eq!(*minimum_chroma_feather, UnitInterval::ZERO);
}

#[test]
fn deserialization_enforces_schema_depth_fanout_and_leaf_bounds() {
    let wrong_schema = r#"{"schema_version":2,"root":{"operator":"leaf","condition":{"kind":"oklab_lightness_range","lower":0.2,"upper":0.8,"softness":0.1}}}"#;
    assert!(
        serde_json::from_str::<ConditionMaskExpression>(wrong_schema)
            .expect_err("schema must fail")
            .to_string()
            .contains("supports schema 1")
    );

    let one_child = ConditionMaskExpression::all(vec![lightness(0.2, 0.8)]);
    assert_eq!(
        one_child,
        Err(RecipeValidationError::InvalidConditionMaskBranchCount {
            operator: "all",
            count: 1,
        })
    );
    let five_children =
        ConditionMaskExpression::any(vec![lightness(0.0, 0.1); MAX_CONDITION_MASK_BRANCHES + 1]);
    assert_eq!(
        five_children,
        Err(RecipeValidationError::InvalidConditionMaskBranchCount {
            operator: "any",
            count: 5,
        })
    );

    let too_deep = ConditionMaskExpression::not(ConditionMaskNode::negated(
        ConditionMaskNode::negated(ConditionMaskNode::negated(lightness(0.2, 0.8))),
    ));
    assert_eq!(
        too_deep,
        Err(RecipeValidationError::ConditionMaskDepthExceeded(5))
    );

    let three_leaves = || {
        ConditionMaskNode::all(vec![
            lightness(0.0, 0.1),
            lightness(0.2, 0.3),
            lightness(0.4, 0.5),
        ])
    };
    let too_many_leaves =
        ConditionMaskExpression::all(vec![three_leaves(), three_leaves(), three_leaves()]);
    assert_eq!(
        too_many_leaves,
        Err(RecipeValidationError::TooManyConditionMaskLeaves(9))
    );
}

#[test]
#[allow(clippy::float_cmp)] // Canonicalization and public normalization constants are exact.
fn color_and_local_detail_conditions_have_explicit_numeric_semantics() {
    let hue = ConditionMaskPredicate::oklch_hue_range(-10.0, 180.0, unit(0.25), unit(0.4))
        .expect("hue range");
    let ConditionMaskPredicate::OklchHueRange {
        center_hue_degrees,
        half_width_degrees,
        minimum_chroma,
        ..
    } = hue
    else {
        panic!("expected hue range");
    };
    assert_eq!(center_hue_degrees.get(), 350.0);
    assert_eq!(half_width_degrees.get(), 180.0);
    assert_eq!(minimum_chroma.get(), 0.25);

    let detail = ConditionMaskPredicate::local_detail_range(12, unit(0.3), unit(0.9), unit(0.2))
        .expect("local detail");
    let encoded = serde_json::to_string(&detail).expect("serialize detail predicate");
    assert_eq!(
        encoded,
        r#"{"kind":"local_detail_range","input":"layer_input_oklab_lightness","algorithm":"box_mean_absolute_residual_v1","radius_level_zero_pixels":12,"lower":0.3,"upper":0.9,"softness":0.2}"#
    );
    assert_eq!(OKLCH_CHROMA_NORMALIZATION, 0.4);
    assert_eq!(LOCAL_DETAIL_RESIDUAL_NORMALIZATION, 0.25);
}

#[test]
fn condition_ranges_reject_reversed_bounds_and_invalid_detail_scale() {
    assert_eq!(
        ConditionMaskExpression::leaf(ConditionMaskPredicate::oklch_chroma_range(
            unit(0.8),
            unit(0.2),
            unit(0.1),
        )),
        Err(RecipeValidationError::InvalidConditionMaskRange {
            kind: "normalized Oklch chroma",
            lower: 0.8,
            upper: 0.2,
        })
    );
    assert_eq!(
        ConditionMaskPredicate::local_detail_range(0, unit(0.2), unit(0.8), unit(0.1)),
        Err(RecipeValidationError::InvalidLocalDetailMaskRadius(0))
    );
}

#[test]
fn presets_deserialize_validated_and_copy_parameters_without_a_live_link() {
    let expression = ConditionMaskExpression::leaf(ConditionMaskPredicate::oklab_lightness_range(
        unit(0.2),
        unit(0.8),
        unit(0.1),
    ))
    .expect("expression");
    let preset =
        ConditionMaskPreset::new("Bright foliage", expression.clone()).expect("valid preset");
    let copied = preset.copy_expression();
    assert_eq!(copied, expression);
    assert_eq!(preset.expression(), &expression);

    let encoded = serde_json::to_string(&preset).expect("serialize preset");
    assert!(!encoded.contains("preset_id"));
    assert!(!encoded.contains("mask_id"));
    assert_eq!(
        serde_json::from_str::<ConditionMaskPreset>(&encoded).expect("deserialize preset"),
        preset
    );
    assert!(
        serde_json::from_str::<ConditionMaskPreset>(
            r#"{"name":" ","expression":{"schema_version":1,"root":{"operator":"leaf","condition":{"kind":"oklab_lightness_range","lower":0.2,"upper":0.8,"softness":0.1}}}}"#
        )
        .is_err()
    );
}

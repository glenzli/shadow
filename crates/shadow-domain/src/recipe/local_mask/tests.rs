use super::*;
use crate::recipe::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, FiniteF64,
    RecipeValidationError, UnitInterval,
};

fn component_id(suffix: u8) -> MaskComponentId {
    format!("00000000-0000-7000-8000-{suffix:012x}")
        .parse()
        .expect("component id")
}

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("unit interval")
}

fn linear_leaf(invert: bool) -> MaskDefinition {
    MaskDefinition::linear_gradient(unit(0.1), unit(0.2), unit(0.8), unit(0.9), invert)
        .expect("linear mask")
}

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
fn one_enabled_base_component_keeps_the_legacy_leaf_byte_shape() {
    let leaf = linear_leaf(true);
    let legacy_bytes = serde_json::to_vec(&leaf).expect("serialize legacy leaf");
    let definition = MaskDefinition::composite(
        vec![
            MaskComponent::new(
                component_id(1),
                MaskComponentOperation::Base,
                true,
                leaf.clone(),
            )
            .expect("base component"),
        ],
        false,
    )
    .expect("normalized composite");

    assert_eq!(definition, leaf);
    assert_eq!(
        serde_json::to_vec(&definition).expect("serialize normalized definition"),
        legacy_bytes
    );

    let direct_composite = MaskDefinition::Composite {
        composite: MaskComposite::new(
            vec![
                MaskComponent::new(
                    component_id(1),
                    MaskComponentOperation::Base,
                    true,
                    linear_leaf(true),
                )
                .expect("base component"),
            ],
            false,
        )
        .expect("intermediate composition"),
    };
    assert_eq!(
        direct_composite.validate(),
        Err(RecipeValidationError::NonCanonicalMaskComposite)
    );
}

#[test]
fn composite_masks_round_trip_stable_components_and_final_inversion() {
    let base = MaskComponent::new(
        component_id(1),
        MaskComponentOperation::Base,
        true,
        linear_leaf(false),
    )
    .expect("base component");
    let subtract = MaskComponent::new(
        component_id(2),
        MaskComponentOperation::Subtract,
        false,
        MaskDefinition::luminance_range(unit(0.25), unit(0.75), unit(0.1), false)
            .expect("luminance range"),
    )
    .expect("subtract component");
    let definition =
        MaskDefinition::composite(vec![base, subtract], true).expect("composite mask definition");
    let encoded = serde_json::to_string(&definition).expect("serialize composite mask");

    assert_eq!(
        encoded,
        r#"{"kind":"composite","components":[{"id":"00000000-0000-7000-8000-000000000001","operation":"base","enabled":true,"definition":{"kind":"linear_gradient","start_x":0.1,"start_y":0.2,"end_x":0.8,"end_y":0.9,"invert":false}},{"id":"00000000-0000-7000-8000-000000000002","operation":"subtract","enabled":false,"definition":{"kind":"luminance_range","lower":0.25,"upper":0.75,"softness":0.1,"invert":false}}],"invert":true}"#
    );
    let decoded =
        serde_json::from_str::<MaskDefinition>(&encoded).expect("deserialize composite mask");
    assert_eq!(decoded, definition);

    let composite = decoded
        .composite_definition()
        .expect("composite definition");
    assert!(composite.invert());
    assert_eq!(composite.components().len(), 2);
    assert_eq!(composite.components()[0].id(), component_id(1));
    assert_eq!(
        composite.components()[1].operation(),
        MaskComponentOperation::Subtract
    );
    assert!(!composite.components()[1].enabled());
    assert!(matches!(
        composite.components()[1].definition(),
        MaskDefinition::LuminanceRange { .. }
    ));
}

#[test]
fn composite_mask_reference_algebra_handles_soft_coverage_bypass_and_inversion() {
    let components = vec![
        MaskComponent::new(
            component_id(1),
            MaskComponentOperation::Base,
            true,
            linear_leaf(false),
        )
        .expect("base"),
        MaskComponent::new(
            component_id(2),
            MaskComponentOperation::Add,
            true,
            linear_leaf(false),
        )
        .expect("add"),
        MaskComponent::new(
            component_id(3),
            MaskComponentOperation::Subtract,
            false,
            linear_leaf(false),
        )
        .expect("disabled subtract"),
        MaskComponent::new(
            component_id(4),
            MaskComponentOperation::Intersect,
            true,
            linear_leaf(false),
        )
        .expect("intersect"),
    ];
    let composite = MaskComposite::new(components, true).expect("composite");
    let coverage = composite
        .reference_coverage(&[unit(0.25), unit(0.5), unit(1.0), unit(0.8)])
        .expect("reference coverage");

    // add(0.25, 0.5) = 0.5; disabled subtract is skipped;
    // intersection with 0.8 = 0.5; final inversion = 0.5.
    assert_eq!(coverage, unit(0.5));
    assert_eq!(
        composite.reference_coverage(&[unit(0.25)]),
        Err(RecipeValidationError::MaskComponentCoverageArity {
            expected: 4,
            actual: 1,
        })
    );

    assert_eq!(
        MaskComponentOperation::Base.combine_coverage(unit(0.9), unit(0.2)),
        unit(0.2)
    );
    assert_eq!(
        MaskComponentOperation::Add.combine_coverage(unit(0.25), unit(0.5)),
        unit(0.5)
    );
    assert_eq!(
        MaskComponentOperation::Subtract.combine_coverage(unit(0.8), unit(0.25)),
        unit(0.75)
    );
    assert_eq!(
        MaskComponentOperation::Intersect.combine_coverage(unit(0.8), unit(0.25)),
        unit(0.25)
    );
    assert_eq!(
        MaskComponentOperation::Add.combine_coverage(unit(0.4), unit(0.4)),
        unit(0.4)
    );
    assert_eq!(
        MaskComponentOperation::Intersect.combine_coverage(unit(0.4), unit(0.4)),
        unit(0.4)
    );
}

#[test]
fn composite_masks_reject_unbounded_ambiguous_or_nested_authored_state() {
    assert_eq!(
        MaskComposite::new(Vec::new(), false),
        Err(RecipeValidationError::EmptyMaskComposite)
    );

    let oversized = (0..=MAX_MASK_COMPONENTS)
        .map(|index| {
            MaskComponent::new(
                component_id((index + 1) as u8),
                if index == 0 {
                    MaskComponentOperation::Base
                } else {
                    MaskComponentOperation::Add
                },
                true,
                linear_leaf(false),
            )
            .expect("component")
        })
        .collect();
    assert_eq!(
        MaskComposite::new(oversized, false),
        Err(RecipeValidationError::TooManyMaskComponents(
            MAX_MASK_COMPONENTS + 1
        ))
    );

    let add_first = MaskComponent::new(
        component_id(1),
        MaskComponentOperation::Add,
        true,
        linear_leaf(false),
    )
    .expect("add component");
    assert_eq!(
        MaskComposite::new(vec![add_first], false),
        Err(RecipeValidationError::FirstMaskComponentMustBeBase)
    );

    let base = MaskComponent::new(
        component_id(1),
        MaskComponentOperation::Base,
        true,
        linear_leaf(false),
    )
    .expect("base component");
    let later_base = MaskComponent::new(
        component_id(2),
        MaskComponentOperation::Base,
        true,
        linear_leaf(false),
    )
    .expect("later base component");
    assert_eq!(
        MaskComposite::new(vec![base.clone(), later_base], false),
        Err(RecipeValidationError::BaseMaskComponentMustBeFirst)
    );

    let duplicate = MaskComponent::new(
        component_id(1),
        MaskComponentOperation::Add,
        true,
        linear_leaf(false),
    )
    .expect("duplicate component");
    assert_eq!(
        MaskComposite::new(vec![base, duplicate], false),
        Err(RecipeValidationError::DuplicateMaskComponent(component_id(
            1
        )))
    );

    let nested_json = r#"{
        "kind":"composite",
        "components":[
          {
            "id":"00000000-0000-7000-8000-000000000001",
            "operation":"base",
            "enabled":true,
            "definition":{
              "kind":"composite",
              "components":[
                {
                  "id":"00000000-0000-7000-8000-000000000002",
                  "operation":"base",
                  "enabled":true,
                  "definition":{
                    "kind":"linear_gradient",
                    "start_x":0.1,
                    "start_y":0.2,
                    "end_x":0.8,
                    "end_y":0.9,
                    "invert":false
                  }
                },
                {
                  "id":"00000000-0000-7000-8000-000000000003",
                  "operation":"add",
                  "enabled":true,
                  "definition":{
                    "kind":"linear_gradient",
                    "start_x":0.2,
                    "start_y":0.3,
                    "end_x":0.7,
                    "end_y":0.8,
                    "invert":false
                  }
                }
              ],
              "invert":false
            }
          },
          {
            "id":"00000000-0000-7000-8000-000000000004",
            "operation":"add",
            "enabled":true,
            "definition":{
              "kind":"linear_gradient",
              "start_x":0.2,
              "start_y":0.3,
              "end_x":0.7,
              "end_y":0.8,
              "invert":false
            }
          }
        ],
        "invert":false
    }"#;
    let error = serde_json::from_str::<MaskDefinition>(nested_json)
        .expect_err("nested composite must be rejected")
        .to_string();
    assert!(error.contains("cannot contain another composite"));
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

#[test]
fn semantic_managed_mask_persists_re_evaluable_intent_without_changing_raster_execution() {
    let content_blake3 = "cd".repeat(32);
    let raster = ManagedRasterMask::new(
        format!(
            "objects/v1/b3/{}/{}",
            &content_blake3[..2],
            &content_blake3[2..]
        ),
        1,
        content_blake3,
        4,
        2,
        2,
        1_024,
        768,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    let intent = SemanticMaskIntent::new("  red   train  ", 4, 30, SemanticMaskAggregation::Union)
        .expect("semantic intent");
    assert_eq!(intent.query(), "red train");
    assert!(
        intent
            .query_revision()
            .starts_with("shadow-semantic-mask-v1:")
    );

    let definition = MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
        raster,
        Some(intent.clone()),
        -12,
        18,
        false,
    )
    .expect("semantic managed mask");
    assert_eq!(definition.semantic_intent(), Some(&intent));
    let encoded = serde_json::to_string(&definition).expect("serialize semantic mask");
    assert!(encoded.contains(r#""semantic_intent":{"contract_version":1"#));
    assert!(encoded.contains(r#""query":"red train""#));
    assert_eq!(
        serde_json::from_str::<MaskDefinition>(&encoded).expect("deserialize semantic mask"),
        definition
    );
}

#[test]
fn semantic_mask_intent_rejects_empty_and_unbounded_requests() {
    assert_eq!(
        SemanticMaskIntent::new("   ", 4, 30, SemanticMaskAggregation::Union),
        Err(RecipeValidationError::EmptySemanticMaskQuery)
    );
    assert_eq!(
        SemanticMaskIntent::new("train", 0, 30, SemanticMaskAggregation::Union),
        Err(RecipeValidationError::InvalidSemanticMaskMaximumRegions(0))
    );
    assert_eq!(
        SemanticMaskIntent::new("train", 4, 0, SemanticMaskAggregation::Union),
        Err(RecipeValidationError::InvalidSemanticMaskScoreThreshold(0))
    );
}

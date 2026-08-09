use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, LiquifyPoint,
    LiquifyStroke, MaskBrushPoint, MaskDefinition, PhotoLiquifyNode, RawFoundationDenoise,
    RawFoundationDenoiseModel, UnitInterval,
};

use crate::ffi;

use super::{
    GradeStackDraft, LOCAL_MASK_BRUSH, LOCAL_MASK_COLOR_RANGE, LOCAL_MASK_LINEAR_GRADIENT,
    LOCAL_MASK_LUMINANCE_RANGE, LOCAL_MASK_MANAGED_RASTER, LOCAL_MASK_RADIAL_GRADIENT,
    PreservedManagedRasterSettings, decode_grade_stack_draft_recipe_v1,
    encode_grade_stack_draft_recipe_v1, ffi_local_mask_fields, local_mask_definition_from_ffi,
    new_basic_grade_node,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

#[test]
fn grade_node_strength_round_trips_and_rejects_invalid_values() {
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].opacity = unit(0.37);
    let mut ffi = encode_grade_stack_draft_recipe_v1(draft).expect("encode strength");
    assert_eq!(ffi.grade_nodes[0].opacity, 0.37);

    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode strength");
    assert_eq!(decoded.grade_nodes[0].opacity, unit(0.37));

    ffi.grade_nodes[0].opacity = 1.01;
    let error = decode_grade_stack_draft_recipe_v1(&ffi).expect_err("reject invalid strength");
    assert!(error.to_string().contains("Grade Node 0 opacity"));
}

#[test]
fn raw_ai_denoise_intent_round_trips_and_unknown_models_fail_closed() {
    let mut ffi =
        encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).expect("default DTO");
    assert!(!ffi.foundation.raw_ai_denoise_present);
    assert!(!ffi.foundation.raw_ai_denoise_enabled);
    assert!(!ffi.foundation.raw_ai_denoise_bypassed);
    assert_eq!(ffi.foundation.raw_ai_denoise_model, 0);
    assert_eq!(ffi.foundation.raw_ai_denoise_amount_percent, 100);

    ffi.foundation.raw_ai_denoise_present = true;
    ffi.foundation.raw_ai_denoise_enabled = true;
    ffi.foundation.raw_ai_denoise_bypassed = true;
    ffi.foundation.raw_ai_denoise_amount_percent = 37;
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode RawNIND intent");
    assert_eq!(
        decoded.raw_ai_denoise,
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0)
            .with_bypassed(true)
            .with_amount_percent(37)
            .expect("valid amount")
    );
    let encoded = encode_grade_stack_draft_recipe_v1(decoded).expect("encode RawNIND intent");
    assert!(encoded.foundation.raw_ai_denoise_present);
    assert!(encoded.foundation.raw_ai_denoise_enabled);
    assert!(encoded.foundation.raw_ai_denoise_bypassed);
    assert_eq!(encoded.foundation.raw_ai_denoise_model, 0);
    assert_eq!(encoded.foundation.raw_ai_denoise_amount_percent, 37);

    let mut bypassed = encoded.clone();
    bypassed.foundation.raw_ai_denoise_enabled = false;
    bypassed.foundation.raw_ai_denoise_bypassed = false;
    let decoded_bypass =
        decode_grade_stack_draft_recipe_v1(&bypassed).expect("decode bypassed RawNIND node");
    assert!(decoded_bypass.raw_ai_denoise.is_present());
    assert!(!decoded_bypass.raw_ai_denoise.is_enabled());
    assert!(!decoded_bypass.raw_ai_denoise.is_bypassed());
    assert_eq!(decoded_bypass.raw_ai_denoise.amount_percent(), 37);

    ffi.foundation.raw_ai_denoise_model = 1;
    let error = decode_grade_stack_draft_recipe_v1(&ffi).expect_err("reject unknown AI model");
    assert!(error.to_string().contains("unsupported AI denoise model 1"));

    ffi.foundation.raw_ai_denoise_model = 0;
    ffi.foundation.raw_ai_denoise_amount_percent = 101;
    let error = decode_grade_stack_draft_recipe_v1(&ffi).expect_err("reject invalid amount");
    assert!(error.to_string().contains("AI RAW denoise amount"));
}

#[test]
fn legacy_local_mask_ffi_slots_remain_exact() {
    let linear = MaskDefinition::linear_gradient(unit(0.1), unit(0.2), unit(0.8), unit(0.9), true)
        .expect("linear mask");
    assert_eq!(
        ffi_local_mask_fields(Some(&linear), None).expect("encode linear"),
        (
            LOCAL_MASK_LINEAR_GRADIENT,
            0.1,
            0.2,
            0.8,
            0.9,
            0.0,
            0.0,
            0.0,
            true,
            Vec::new(),
        ),
    );

    let radial = MaskDefinition::radial_gradient(
        unit(0.4),
        unit(0.6),
        unit(0.2),
        unit(0.3),
        unit(0.5),
        false,
    )
    .expect("radial mask");
    assert_eq!(
        ffi_local_mask_fields(Some(&radial), None).expect("encode radial"),
        (
            LOCAL_MASK_RADIAL_GRADIENT,
            0.4,
            0.6,
            0.0,
            0.0,
            0.2,
            0.3,
            0.5,
            false,
            Vec::new(),
        ),
    );

    let brush = MaskDefinition::brush(
        vec![MaskBrushPoint::new(unit(0.25), unit(0.75), true)],
        unit(0.04),
        unit(0.6),
        true,
    )
    .expect("brush mask");
    assert_eq!(
        ffi_local_mask_fields(Some(&brush), None).expect("encode brush"),
        (
            LOCAL_MASK_BRUSH,
            0.0,
            0.0,
            0.0,
            0.0,
            0.04,
            0.0,
            0.6,
            true,
            vec![0.25, 0.75, 1.0],
        ),
    );
}

#[test]
#[allow(clippy::float_cmp)] // This test pins the normalized desktop FFI slots exactly.
fn condition_masks_round_trip_through_normalized_desktop_slots() {
    let luminance = MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.15), true)
        .expect("luminance range");
    assert_eq!(
        ffi_local_mask_fields(Some(&luminance), None).expect("encode luminance"),
        (
            LOCAL_MASK_LUMINANCE_RANGE,
            0.2,
            0.0,
            0.8,
            0.0,
            0.0,
            0.0,
            0.15,
            true,
            Vec::new(),
        ),
    );

    let color = MaskDefinition::color_range(270.0, 45.0, unit(0.4), false).expect("color range");
    assert_eq!(
        ffi_local_mask_fields(Some(&color), None).expect("encode color"),
        (
            LOCAL_MASK_COLOR_RANGE,
            0.75,
            0.0,
            0.25,
            0.0,
            0.0,
            0.0,
            0.4,
            false,
            Vec::new(),
        ),
    );

    let mut grade_node = new_basic_grade_node("Condition mask").expect("neutral Grade Node");
    grade_node.local_mask_kind = LOCAL_MASK_LUMINANCE_RANGE;
    grade_node.local_mask_x0 = 0.2;
    grade_node.local_mask_x1 = 0.8;
    grade_node.local_mask_feather = 0.15;
    grade_node.local_mask_invert = true;
    assert_eq!(
        local_mask_definition_from_ffi(&grade_node, 0).expect("decode luminance range"),
        (Some(luminance), None)
    );

    grade_node.local_mask_kind = LOCAL_MASK_COLOR_RANGE;
    grade_node.local_mask_x0 = 0.75;
    grade_node.local_mask_x1 = 0.25;
    grade_node.local_mask_feather = 0.4;
    grade_node.local_mask_invert = false;
    assert_eq!(
        local_mask_definition_from_ffi(&grade_node, 0).expect("decode color range"),
        (Some(color), None)
    );

    grade_node.local_mask_x0 = 1.0;
    let (wrapped, preserved) = local_mask_definition_from_ffi(&grade_node, 0)
        .expect("normalized endpoint wraps to canonical hue");
    assert_eq!(preserved, None);
    let wrapped = wrapped.expect("color range");
    let MaskDefinition::ColorRange {
        center_hue_degrees, ..
    } = wrapped
    else {
        panic!("expected color range")
    };
    assert_eq!(center_hue_degrees.get(), 0.0);
}

#[test]
fn malformed_condition_mask_slots_fail_closed() {
    let mut grade_node = new_basic_grade_node("Invalid mask").expect("neutral Grade Node");
    grade_node.local_mask_kind = LOCAL_MASK_LUMINANCE_RANGE;
    grade_node.local_mask_x0 = 0.8;
    grade_node.local_mask_x1 = 0.2;
    grade_node.local_mask_feather = 0.1;
    assert!(local_mask_definition_from_ffi(&grade_node, 0).is_err());

    grade_node.local_mask_kind = LOCAL_MASK_COLOR_RANGE;
    grade_node.local_mask_x0 = 0.5;
    grade_node.local_mask_x1 = 0.0;
    assert!(local_mask_definition_from_ffi(&grade_node, 0).is_err());

    grade_node.local_mask_x1 = 0.2;
    grade_node.local_mask_feather = f64::NAN;
    assert!(local_mask_definition_from_ffi(&grade_node, 0).is_err());
}

#[test]
fn managed_raster_is_an_opaque_kind_six_marker_with_refinement() {
    assert_eq!(
        ffi_local_mask_fields(
            None,
            Some(PreservedManagedRasterSettings {
                expansion_percent: -35,
                feather_percent: 24,
                invert: true,
            })
        )
        .expect("encode opaque managed raster"),
        (
            LOCAL_MASK_MANAGED_RASTER,
            -0.35,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.24,
            true,
            Vec::new(),
        )
    );
    let mut grade_node = new_basic_grade_node("Managed mask").expect("neutral Grade Node");
    grade_node.local_mask_kind = LOCAL_MASK_MANAGED_RASTER;
    grade_node.local_mask_x0 = 0.18;
    grade_node.local_mask_feather = 0.31;
    grade_node.local_mask_invert = true;
    assert_eq!(
        local_mask_definition_from_ffi(&grade_node, 0).expect("decode opaque managed raster"),
        (
            None,
            Some(PreservedManagedRasterSettings {
                expansion_percent: 18,
                feather_percent: 31,
                invert: true,
            })
        )
    );

    grade_node.local_mask_x0 = 0.185;
    assert!(
        local_mask_definition_from_ffi(&grade_node, 0)
            .expect_err("sub-percent managed refinement is not canonical")
            .to_string()
            .contains("one-percent increments")
    );
}

#[test]
fn current_qt_dto_rejects_persisted_composite_condition_masks() {
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
    let definition =
        MaskDefinition::condition_expression(expression).expect("persistent condition mask");
    let error = ffi_local_mask_fields(Some(&definition), None)
        .expect_err("DTO must reject unsupported shape");
    assert!(
        error
            .to_string()
            .contains("current Qt Grade Node DTO cannot represent")
    );

    let mut grade_stack = GradeStackDraft::default();
    grade_stack.grade_nodes[0].local_mask = Some(definition);
    let projection_error = encode_grade_stack_draft_recipe_v1(grade_stack)
        .expect_err("complete desktop projection must return the unsupported condition");
    assert!(
        projection_error
            .to_string()
            .contains("current Qt Grade Node DTO cannot represent")
    );
}

#[test]
#[allow(clippy::float_cmp)] // The normalized FFI slots are an exact persistence boundary.
fn ordered_bypassed_liquify_round_trips_exactly_through_the_desktop_dto() {
    let push = LiquifyStroke::push(
        vec![
            LiquifyPoint::with_pressure(unit(0.2), unit(0.3), unit(0.4)),
            LiquifyPoint::with_pressure(unit(0.6), unit(0.7), unit(0.8)),
        ],
        unit(0.12),
        unit(0.55),
        unit(0.72),
    )
    .expect("valid push stroke");
    let reconstruct = LiquifyStroke::reconstruct(
        vec![LiquifyPoint::with_pressure(
            unit(0.5),
            unit(0.45),
            unit(0.65),
        )],
        unit(0.09),
        unit(0.35),
        unit(0.4),
    )
    .expect("valid reconstruct stroke");
    let node = PhotoLiquifyNode::new(vec![push, reconstruct])
        .expect("ordered Liquify")
        .with_enabled(false);
    let draft = GradeStackDraft {
        liquify: Some(node.clone()),
        ..GradeStackDraft::default()
    };

    let wire = encode_grade_stack_draft_recipe_v1(draft).expect("encode Liquify");
    assert!(!wire.liquify_enabled);
    assert_eq!(wire.liquify_strokes.len(), 2);
    assert_eq!(wire.liquify_strokes[0].kind, 0);
    assert_eq!(wire.liquify_strokes[1].kind, 1);
    assert_eq!(wire.liquify_strokes[0].points.len(), 2);
    assert_eq!(wire.liquify_strokes[0].points[0].x, 0.2);
    assert_eq!(wire.liquify_strokes[0].points[0].y, 0.3);
    assert_eq!(wire.liquify_strokes[0].points[0].pressure, 0.4);
    assert_eq!(wire.liquify_strokes[0].radius, 0.12);
    assert_eq!(wire.liquify_strokes[0].strength, 0.55);
    assert_eq!(wire.liquify_strokes[0].hardness, 0.72);
    assert_eq!(wire.liquify_strokes[1].points.len(), 1);
    assert_eq!(wire.liquify_strokes[1].points[0].pressure, 0.65);

    let decoded = decode_grade_stack_draft_recipe_v1(&wire).expect("decode Liquify");
    assert_eq!(decoded.liquify, Some(node));

    let absent =
        encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).expect("encode absence");
    assert!(absent.liquify_strokes.is_empty());
}

#[test]
fn malformed_liquify_strokes_fail_closed_at_the_desktop_boundary() {
    let mut wire =
        encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).expect("neutral wire");
    wire.liquify_strokes.push(ffi::FfiLiquifyStroke {
        kind: 0,
        points: vec![ffi::FfiLiquifyPoint {
            x: 0.5,
            y: 0.5,
            pressure: 1.0,
        }],
        radius: 0.1,
        strength: 0.5,
        hardness: 0.5,
    });
    assert!(
        decode_grade_stack_draft_recipe_v1(&wire)
            .expect_err("one-point Liquify stroke must fail")
            .to_string()
            .contains("Liquify stroke 0 is invalid")
    );
}

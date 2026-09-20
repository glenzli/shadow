use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, LiquifyPoint,
    LiquifyStroke, MaskBrushPoint, MaskDefinition, PhotoLiquifyNode, RawFoundationDenoise,
    RawFoundationDenoiseModel, RetouchPoint, RetouchStroke, SemanticMaskAggregation,
    SemanticMaskIntent, UnitInterval,
};

use crate::ffi;

use super::{
    GradeStackDraft, LOCAL_MASK_BRUSH, LOCAL_MASK_COLOR_RANGE, LOCAL_MASK_LINEAR_GRADIENT,
    LOCAL_MASK_LUMINANCE_RANGE, LOCAL_MASK_MANAGED_RASTER, LOCAL_MASK_RADIAL_GRADIENT,
    PreservedManagedRasterSettings, decode_grade_stack_draft_recipe_v1,
    encode_grade_stack_draft_recipe_v1, ffi_local_mask_fields, local_mask_definition_from_ffi,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

fn ffi_mask_component(kind: u8) -> ffi::FfiMaskComponent {
    ffi::FfiMaskComponent {
        component_id: "00000000-0000-0000-0000-000000000001".to_owned(),
        operation: 0,
        enabled: true,
        kind,
        x0: 0.0,
        y0: 0.0,
        x1: 0.0,
        y1: 0.0,
        radius_x: 0.0,
        radius_y: 0.0,
        feather: 0.0,
        leaf_invert: false,
        brush_points: Vec::new(),
        semantic_query: String::new(),
        semantic_maximum_regions: 0,
        semantic_score_threshold_percent: 0,
    }
}

#[test]
fn legacy_leaf_component_identity_survives_repeated_projection_and_autosave() {
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].local_mask = Some(
        MaskDefinition::linear_gradient(unit(0.1), unit(0.2), unit(0.8), unit(0.9), false)
            .expect("legacy leaf"),
    );
    let snapshot = super::grade_stack_recipe_v1_snapshot(&draft, None).expect("persist leaf");
    let first = encode_grade_stack_draft_recipe_v1(draft.clone()).expect("first projection");
    let second = encode_grade_stack_draft_recipe_v1(draft).expect("second projection");
    let identity = &first.grade_nodes[0].local_mask_components[0].component_id;
    assert_eq!(
        identity,
        &second.grade_nodes[0].local_mask_components[0].component_id
    );
    let restored = decode_grade_stack_draft_recipe_v1(&first).expect("decode desktop draft");
    let saved =
        super::grade_stack_recipe_v1_snapshot(&restored, Some(&snapshot)).expect("autosave");
    let reopened = crate::recipe_v1::decode_grade_stack_draft_from_recipe_v1_snapshot(&saved)
        .expect("reopen saved snapshot");
    let reopened = encode_grade_stack_draft_recipe_v1(reopened).expect("reproject");
    assert_eq!(
        identity,
        &reopened.grade_nodes[0].local_mask_components[0].component_id
    );
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

    let mut component = ffi_mask_component(LOCAL_MASK_LUMINANCE_RANGE);
    component.x0 = 0.2;
    component.x1 = 0.8;
    component.feather = 0.15;
    component.leaf_invert = true;
    assert_eq!(
        local_mask_definition_from_ffi(&component, 0, 0).expect("decode luminance range"),
        (Some(luminance), None)
    );

    component.kind = LOCAL_MASK_COLOR_RANGE;
    component.x0 = 0.75;
    component.x1 = 0.25;
    component.feather = 0.4;
    component.leaf_invert = false;
    assert_eq!(
        local_mask_definition_from_ffi(&component, 0, 0).expect("decode color range"),
        (Some(color), None)
    );

    component.x0 = 1.0;
    let (wrapped, preserved) = local_mask_definition_from_ffi(&component, 0, 0)
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
    let mut component = ffi_mask_component(LOCAL_MASK_LUMINANCE_RANGE);
    component.x0 = 0.8;
    component.x1 = 0.2;
    component.feather = 0.1;
    assert!(local_mask_definition_from_ffi(&component, 0, 0).is_err());

    component.kind = LOCAL_MASK_COLOR_RANGE;
    component.x0 = 0.5;
    component.x1 = 0.0;
    assert!(local_mask_definition_from_ffi(&component, 0, 0).is_err());

    component.x1 = 0.2;
    component.feather = f64::NAN;
    assert!(local_mask_definition_from_ffi(&component, 0, 0).is_err());
}

#[test]
fn managed_raster_is_an_opaque_kind_six_marker_with_refinement() {
    assert_eq!(
        ffi_local_mask_fields(
            None,
            Some(&PreservedManagedRasterSettings {
                expansion_percent: -35,
                feather_percent: 24,
                invert: true,
                semantic_intent: None,
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
    let mut component = ffi_mask_component(LOCAL_MASK_MANAGED_RASTER);
    component.x0 = 0.18;
    component.feather = 0.31;
    component.leaf_invert = true;
    assert_eq!(
        local_mask_definition_from_ffi(&component, 0, 0).expect("decode opaque managed raster"),
        (
            None,
            Some(PreservedManagedRasterSettings {
                expansion_percent: 18,
                feather_percent: 31,
                invert: true,
                semantic_intent: None,
            })
        )
    );

    component.x0 = 0.185;
    assert!(
        local_mask_definition_from_ffi(&component, 0, 0)
            .expect_err("sub-percent managed refinement is not canonical")
            .to_string()
            .contains("one-percent increments")
    );
}

#[test]
fn semantic_managed_raster_intent_survives_the_qt_dto() {
    let intent = SemanticMaskIntent::new("red train", 4, 30, SemanticMaskAggregation::Union)
        .expect("semantic intent");
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].local_mask = None;
    draft.grade_nodes[0].preserved_managed_raster = Some(PreservedManagedRasterSettings {
        expansion_percent: 0,
        feather_percent: 0,
        invert: false,
        semantic_intent: Some(intent.clone()),
    });

    let ffi = encode_grade_stack_draft_recipe_v1(draft).expect("encode semantic managed mask");
    assert_eq!(ffi.grade_nodes[0].local_mask_components.len(), 1);
    assert_eq!(
        ffi.grade_nodes[0].local_mask_components[0].semantic_query,
        "red train"
    );
    assert_eq!(
        ffi.grade_nodes[0].local_mask_components[0].semantic_maximum_regions,
        4
    );
    assert_eq!(
        ffi.grade_nodes[0].local_mask_components[0].semantic_score_threshold_percent,
        30
    );
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode semantic managed mask");
    assert_eq!(
        decoded.grade_nodes[0]
            .composite_mask
            .as_ref()
            .and_then(|composite| composite.components.first())
            .and_then(|component| match &component.definition {
                super::MaskComponentDraftDefinition::PreservedManagedRaster(settings) => {
                    settings.semantic_intent.as_ref()
                }
                super::MaskComponentDraftDefinition::Definition(_) => None,
            }),
        Some(&intent)
    );
}

#[test]
fn typed_mask_components_round_trip_stable_identity_order_bypass_and_final_inversion() {
    let mut ffi =
        encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).expect("default DTO");
    let mut base = ffi_mask_component(LOCAL_MASK_LINEAR_GRADIENT);
    base.component_id = "00000000-0000-0000-0000-000000000011".to_owned();
    base.x0 = 0.1;
    base.y0 = 0.2;
    base.x1 = 0.8;
    base.y1 = 0.9;
    let mut subtract = ffi_mask_component(LOCAL_MASK_LUMINANCE_RANGE);
    subtract.component_id = "00000000-0000-0000-0000-000000000012".to_owned();
    subtract.operation = 2;
    subtract.enabled = false;
    subtract.x0 = 0.3;
    subtract.x1 = 0.7;
    subtract.feather = 0.1;
    ffi.grade_nodes[0].local_mask_components = vec![base, subtract];
    ffi.grade_nodes[0].local_mask_invert = true;

    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode composite DTO");
    let encoded = encode_grade_stack_draft_recipe_v1(decoded).expect("re-encode composite DTO");
    let components = &encoded.grade_nodes[0].local_mask_components;
    assert_eq!(components.len(), 2);
    assert_eq!(
        components[0].component_id,
        "00000000-0000-0000-0000-000000000011"
    );
    assert_eq!(components[0].operation, 0);
    assert!(components[0].enabled);
    assert_eq!(
        components[1].component_id,
        "00000000-0000-0000-0000-000000000012"
    );
    assert_eq!(components[1].operation, 2);
    assert!(!components[1].enabled);
    assert!(encoded.grade_nodes[0].local_mask_invert);
}

#[test]
fn typed_mask_components_fail_closed_on_unknown_or_ambiguous_topology() {
    let mut ffi =
        encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).expect("default DTO");
    let mut base = ffi_mask_component(LOCAL_MASK_LINEAR_GRADIENT);
    base.x0 = 0.1;
    base.y0 = 0.2;
    base.x1 = 0.8;
    base.y1 = 0.9;
    ffi.grade_nodes[0].local_mask_components = vec![base.clone()];

    ffi.grade_nodes[0].local_mask_components[0].operation = 9;
    assert!(decode_grade_stack_draft_recipe_v1(&ffi).is_err());
    ffi.grade_nodes[0].local_mask_components[0].operation = 0;
    ffi.grade_nodes[0].local_mask_components[0].kind = 99;
    assert!(decode_grade_stack_draft_recipe_v1(&ffi).is_err());
    ffi.grade_nodes[0].local_mask_components[0].kind = LOCAL_MASK_LINEAR_GRADIENT;
    let mut duplicate = base;
    duplicate.operation = 1;
    ffi.grade_nodes[0].local_mask_components.push(duplicate);
    assert!(decode_grade_stack_draft_recipe_v1(&ffi).is_err());
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
fn bypassed_retouch_node_round_trips_without_discarding_regions() {
    let mut draft = GradeStackDraft::default();
    draft.retouch_strokes = vec![
        RetouchStroke::new(vec![RetouchPoint::new(unit(0.4), unit(0.6))], 24)
            .expect("one-point Repair region"),
    ];
    draft.retouch_enabled = false;

    let wire = encode_grade_stack_draft_recipe_v1(draft).expect("encode Repair node");
    assert!(!wire.retouch_enabled);
    assert_eq!(wire.retouch_strokes.len(), 1);

    let decoded = decode_grade_stack_draft_recipe_v1(&wire).expect("decode Repair node");
    assert!(!decoded.retouch_enabled);
    assert_eq!(decoded.retouch_strokes.len(), 1);
    assert_eq!(decoded.retouch_strokes[0].points().len(), 1);
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

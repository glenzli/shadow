//! Basic, cross-family fine-edit, curve, LUT, and non-color FFI contracts.

use shadow_bridge::{
    AdjustmentRenderOperation, BasicEditParameters, ColorRangeParameters,
    OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT, OklabColorWarperControlPoint,
    OklabColorWarperParameters, OklabLightnessToneCurve, PerceptualColorParameters,
    SELECTIVE_COLOR_VALUE_COUNT, SelectiveToneParameters, SharpenParameters, ToneCurvePoint,
};
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, BASIC_LAYER_LABEL, FINISHING_EFFECTS_OPERATION_ID,
    LUT_3D_OPERATION_ID,
};
use shadow_domain::{
    AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditGraph, ImageDomain,
    LayerContent, LayerInstance, PhotoFoundationNode, PhotoGeometry, PortType, RawTemperatureTint,
    RawWhiteBalance, RecipeInputSettings, RecipeSnapshot, UnitInterval, diff_recipe_snapshots,
};
use uuid::Uuid;

use crate::{
    edit_version_diff::{changed_grade_parameters_recipe_v1, has_other_recipe_changes},
    recipe_v1::{
        FineEditParameters, GradeNodeDraft, GradeStackDraft, LutEditParameters,
        basic_parameters_from_snapshot, basic_recipe_snapshot, compile_recipe_render_plan,
        decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
        encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
        preview_grade_stack_draft_recipe_v1, single_grade_node_recipe_v1_render_ops,
    },
    tests::fixtures::grade_stack::ffi_parameters,
};

#[test]
fn basic_recipe_round_trip_preserves_renderer_parameters() {
    let expected = BasicEditParameters {
        exposure_stops: 1.25,
        contrast_factor: 1.4,
        white_balance_temperature: 0.2,
        white_balance_tint: -0.05,
        saturation_factor: 0.75,
    };

    let snapshot = basic_recipe_snapshot(expected, None).expect("build basic Recipe");
    let actual = basic_parameters_from_snapshot(&snapshot).expect("read basic Recipe");

    assert_eq!(actual, expected);
}

#[test]
#[allow(clippy::too_many_lines)]
fn fine_edit_round_trip_preserves_every_parameter_and_execution_slot() {
    let expected = FineEditParameters {
        selective_tone: SelectiveToneParameters {
            highlights: -0.35,
            shadows: 0.4,
            whites: 0.15,
            blacks: -0.2,
        },
        perceptual_color: PerceptualColorParameters {
            global_a_balance: -0.28,
            global_b_balance: 0.19,
            vibrance: 0.3,
            hue_shifts: [-0.4, -0.3, -0.2, -0.1, 0.1, 0.2, 0.3, 0.4],
            saturation: [0.45, 0.35, 0.25, 0.15, -0.15, -0.25, -0.35, -0.45],
            lightness: [-0.5, -0.25, 0.0, 0.25, 0.5, 0.25, 0.0, -0.25],
            color_range: ColorRangeParameters {
                enabled: true,
                center_hue_degrees: 359.5,
                width_degrees: 72.0,
                softness: 0.65,
                hue_shift_degrees: -42.0,
                saturation: 0.55,
                lightness: -0.3,
            },
            additional_color_ranges: vec![ColorRangeParameters {
                enabled: true,
                center_hue_degrees: 145.0,
                width_degrees: 24.0,
                softness: 0.4,
                hue_shift_degrees: 8.0,
                saturation: -0.2,
                lightness: 0.1,
            }],
            selective_color_relative: false,
            selective_color_lightness_protection: 0.35,
            selective_color_cmyk: [0.2; SELECTIVE_COLOR_VALUE_COUNT],
        },
        oklab_color_warper: OklabColorWarperParameters {
            control_points: [OklabColorWarperControlPoint {
                a_offset: 0.06,
                b_offset: -0.04,
            }; OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT],
            strength: 0.72,
        },
        oklab_lightness_curve: Some(OklabLightnessToneCurve {
            lightness: vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 0.45, y: 0.62 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
        }),
        lut: LutEditParameters::default(),
        sharpen: SharpenParameters {
            amount: 1.35,
            radius: 2.4,
            threshold: 0.18,
            masking: 0.72,
            local_contrast: -0.52,
            local_contrast_scale: 0.70,
            denoise_luminance: 0.3,
            dehaze: 0.2,
            shadows_hue: 220.0,
            shadows_saturation: 0.18,
            grain_amount: 0.12,
            vignette_amount: -0.2,
            ..SharpenParameters::default()
        },
    };
    let grade_stack = GradeStackDraft {
        raw_ai_denoise: shadow_domain::RawFoundationDenoise::disabled(),
        foundation: PhotoFoundationNode::default(),
        grade_nodes: vec![GradeNodeDraft {
            fine: expected.clone(),
            ..GradeNodeDraft::neutral(BASIC_LAYER_LABEL)
        }],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        liquify: None,
        geometry: PhotoGeometry::identity(),
    };

    let ffi_round_trip = decode_grade_stack_draft_recipe_v1(
        &encode_grade_stack_draft_recipe_v1(grade_stack.clone()).expect("encode Grade Stack"),
    )
    .expect("FFI fine controls round trip");
    assert_eq!(ffi_round_trip.fine, expected);

    let snapshot =
        grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist fine controls");
    let persisted = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode persisted fine controls");
    assert_eq!(persisted.fine, expected);

    let plan = compile_recipe_render_plan(&snapshot).expect("compile fine controls");
    assert_eq!(plan.nodes.len(), 12);
    assert!(matches!(
        plan.nodes[3].operation,
        AdjustmentRenderOperation::SelectiveTone { parameters }
            if parameters == expected.selective_tone
    ));
    assert!(matches!(
        &plan.nodes[5].operation,
        AdjustmentRenderOperation::PerceptualColor { parameters }
            if parameters.as_ref() == &expected.perceptual_color
    ));
    assert!(matches!(
        &plan.nodes[6].operation,
        AdjustmentRenderOperation::OklabColorWarper { parameters }
            if parameters.as_ref() == &expected.oklab_color_warper
    ));
    assert!(matches!(
        &plan.nodes[7].operation,
        AdjustmentRenderOperation::OklabLightnessToneCurve { curve }
            if curve.as_ref() == expected.oklab_lightness_curve.as_ref().unwrap()
    ));
    assert!(matches!(
        &plan.nodes[8].operation,
        AdjustmentRenderOperation::Sharpen { parameters, .. }
            if parameters.as_ref() == &expected.sharpen
    ));
    assert!(matches!(
        &plan.nodes[9].operation,
        AdjustmentRenderOperation::Sharpen { parameters, .. }
            if parameters.as_ref() == &expected.sharpen
    ));
    assert!(matches!(
        &plan.nodes[11].operation,
        AdjustmentRenderOperation::Sharpen { parameters, .. }
            if parameters.as_ref() == &expected.sharpen
    ));
}

#[test]
fn oklab_lightness_curve_has_one_stable_slot_and_rejects_invalid_geometry() {
    let mut draft = GradeStackDraft::default();
    let curve = OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.02 },
            ToneCurvePoint { x: 0.5, y: 0.68 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    };
    draft.fine.oklab_lightness_curve = Some(curve.clone());
    let identity = draft.recipe_v1_identity.oklab_lightness_curve_render_op_id;
    let snapshot =
        grade_stack_recipe_v1_snapshot(&draft, None).expect("persist perceptual lightness curve");
    let recipe_nodes = single_grade_node_recipe_v1_render_ops(&snapshot)
        .expect("read current Grade Node render operations");
    assert_eq!(
        recipe_nodes
            .oklab_lightness_curve
            .expect("Oklab curve render operation")
            .id(),
        identity
    );
    let plan = compile_recipe_render_plan(&snapshot).expect("compile perceptual curve");
    assert!(matches!(
        &plan.nodes[6].operation,
        AdjustmentRenderOperation::OklabLightnessToneCurve { curve: compiled }
            if compiled.as_ref() == &curve
    ));

    let encoded = encode_grade_stack_draft_recipe_v1(draft).expect("encode Grade Stack");
    assert_eq!(
        encoded.grade_nodes[0].fine.oklab_lightness_curve_points,
        vec![0.0, 0.02, 0.5, 0.68, 1.0, 1.0]
    );

    let mut invalid = encoded;
    invalid.grade_nodes[0].fine.oklab_lightness_curve_points =
        vec![0.0, 0.0, 0.5, 0.4, 0.5, 0.7, 1.0, 1.0];
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid)
            .expect_err("a perceptual curve cannot have duplicate x coordinates")
            .to_string()
            .contains("strictly increasing")
    );
}

#[test]
fn oklab_lightness_curve_versions_report_only_the_perceptual_control() {
    let before = GradeStackDraft::default();
    let mut after = before.clone();
    after.fine.oklab_lightness_curve = Some(OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.45, y: 0.62 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    });
    assert_eq!(
        changed_grade_parameters_recipe_v1(&before, &after),
        ["oklab_lightness_curve"]
    );
    let before_snapshot =
        grade_stack_recipe_v1_snapshot(&before, None).expect("persist neutral Grade Node");
    let after_snapshot = grade_stack_recipe_v1_snapshot(&after, Some(&before_snapshot))
        .expect("persist Oklab curve edit");
    assert!(!has_other_recipe_changes(
        &diff_recipe_snapshots(&before_snapshot, &after_snapshot),
        &before_snapshot,
        &after_snapshot,
    ));
}

#[test]
fn absolute_foundation_white_balance_is_not_reported_as_grade_temperature_or_tint() {
    let before = GradeStackDraft::default();
    let mut after = before.clone();
    after.foundation = PhotoFoundationNode::new(
        RecipeInputSettings::new(after.foundation.optics().clone()).with_raw_white_balance(
            RawWhiteBalance::temperature_tint(
                RawTemperatureTint::new(5_800, 9).expect("manual temperature/tint"),
            ),
        ),
    );

    assert!(
        changed_grade_parameters_recipe_v1(&before, &after).is_empty(),
        "absolute Foundation white balance is not a Grade parameter"
    );
    let before_snapshot =
        grade_stack_recipe_v1_snapshot(&before, None).expect("persist As Shot Foundation");
    let after_snapshot =
        grade_stack_recipe_v1_snapshot(&after, None).expect("persist manual Foundation");
    assert!(has_other_recipe_changes(
        &diff_recipe_snapshots(&before_snapshot, &after_snapshot),
        &before_snapshot,
        &after_snapshot,
    ));
}

#[test]
fn managed_lut_round_trips_and_compiles_the_exact_document_and_strength() {
    let root = std::env::temp_dir().join(format!(
        "shadow-managed-lut-test-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    std::fs::create_dir_all(&root).expect("create LUT fixture root");
    let resource_id = "a".repeat(64);
    let path = root.join(format!("{resource_id}.cube"));
    let document = b"LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    std::fs::write(&path, document).expect("write managed LUT fixture");

    let mut grade_node = GradeNodeDraft::neutral(BASIC_LAYER_LABEL);
    grade_node.fine.lut = LutEditParameters {
        resource_id: resource_id.clone(),
        title: "Identity test LUT".to_owned(),
        managed_path: path.to_str().expect("UTF-8 LUT path").to_owned(),
        intensity: 0.37,
    };
    let grade_stack = GradeStackDraft {
        raw_ai_denoise: shadow_domain::RawFoundationDenoise::disabled(),
        foundation: PhotoFoundationNode::default(),
        grade_nodes: vec![grade_node],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        liquify: None,
        geometry: PhotoGeometry::identity(),
    };
    let snapshot =
        grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist managed LUT selection");
    let round_trip = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode managed LUT selection");
    assert_eq!(round_trip.fine.lut, grade_stack.fine.lut);

    let plan = compile_recipe_render_plan(&snapshot).expect("compile managed LUT");
    assert!(matches!(
        &plan.nodes[8].operation,
        AdjustmentRenderOperation::Lut3D {
            document: compiled,
            intensity,
        } if compiled == document && (*intensity - 0.37).abs() < f64::EPSILON
    ));

    std::fs::remove_dir_all(root).expect("remove LUT fixture root");
}

#[test]
fn fine_edit_ffi_validation_rejects_non_color_out_of_range_values() {
    let mut invalid_tone = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_tone.fine.highlights = 1.01;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_tone)
            .expect_err("out-of-range highlights must fail closed")
            .to_string()
            .contains("highlights")
    );

    let mut invalid_sharpen = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_sharpen.fine.sharpen_radius = 0.0;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_sharpen)
            .expect_err("zero sharpen radius must fail closed")
            .to_string()
            .contains("sharpen radius")
    );

    let mut invalid_local_contrast = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_local_contrast.fine.local_contrast_scale = 1.01;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_local_contrast)
            .expect_err("Local Contrast scale must fail closed outside its unit interval")
            .to_string()
            .contains("local contrast scale")
    );
}

#[test]
fn incomplete_recipe_shapes_are_rejected_instead_of_upgraded() {
    let without_finishing = recipe_without_finishing_effects(
        &grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("new neutral Recipe"),
    );
    let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&without_finishing)
        .expect_err("a Recipe missing the required finishing node must fail closed");
    assert!(!format!("{error:#}").is_empty());

    let mut curved_draft = GradeStackDraft::default();
    curved_draft.fine.oklab_lightness_curve = Some(OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.5, y: 0.65 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    });
    let curved_without_finishing = recipe_without_finishing_effects(
        &grade_stack_recipe_v1_snapshot(&curved_draft, None).expect("new curved Recipe"),
    );
    let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&curved_without_finishing)
        .expect_err("an ambiguous incomplete Recipe must fail closed");
    assert!(!format!("{error:#}").is_empty());
}

#[test]
fn neutral_before_ignores_transient_edit_settings() {
    let mut non_neutral = ffi_parameters(2.0, 1.7, [0.2, -0.1], 0.6);
    non_neutral.grade_nodes[0].fine.oklab_lightness_curve_points =
        vec![0.0, 0.1, 0.5, 0.8, 1.0, 1.0];
    non_neutral.enabled = false;

    let before =
        preview_grade_stack_draft_recipe_v1(&non_neutral, false).expect("select neutral Before");
    assert_eq!(before.grade_nodes.len(), 1);
    assert_eq!(before.basic, BasicEditParameters::default());
    assert!(before.fine.oklab_lightness_curve.is_none());
    assert!(before.enabled);
    let current =
        preview_grade_stack_draft_recipe_v1(&non_neutral, true).expect("select current parameters");
    assert_ne!(current.basic, BasicEditParameters::default());
    assert!(current.fine.oklab_lightness_curve.is_some());
    assert!(!current.enabled);
}

fn recipe_without_finishing_effects(snapshot: &RecipeSnapshot) -> RecipeSnapshot {
    let [layer] = snapshot.layers() else {
        panic!("finishing compatibility fixture requires exactly one layer");
    };
    let LayerContent::Inline { graph } = layer.content() else {
        panic!("finishing compatibility fixture requires an inline graph");
    };
    let nodes = graph
        .nodes()
        .iter()
        .filter(|node| node.operation().operation_id().as_str() != FINISHING_EFFECTS_OPERATION_ID)
        .cloned()
        .collect::<Vec<_>>();
    let output = nodes
        .iter()
        .find(|node| node.operation().operation_id().as_str() == LUT_3D_OPERATION_ID)
        .expect("complete Recipe contains LUT")
        .id();
    let graph = EditGraph::new(
        BASIC_GRAPH_SCHEMA_VERSION,
        vec![PortType::Image(ImageDomain::WorkingRgb)],
        nodes,
        output,
    )
    .expect("build incomplete compatibility graph");
    RecipeSnapshot::new(
        CURRENT_RECIPE_SCHEMA_VERSION,
        vec![
            LayerInstance::new(
                layer.id(),
                layer.label(),
                AdjustmentScope::Photo,
                LayerContent::Inline { graph },
                layer.enabled(),
                UnitInterval::ONE,
                BlendMode::Normal,
                None,
            )
            .expect("build incomplete compatibility layer"),
        ],
    )
    .expect("build incomplete compatibility Recipe")
}

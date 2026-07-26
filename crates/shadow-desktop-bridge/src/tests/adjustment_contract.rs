//! Basic, fine-control, curve, LUT, and FFI adjustment contracts.

use super::*;

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
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![GradeNodeDraft {
            fine: expected.clone(),
            ..GradeNodeDraft::neutral(BASIC_LAYER_LABEL)
        }],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };

    let ffi_round_trip = decode_grade_stack_draft_recipe_v1(&encode_grade_stack_draft_recipe_v1(
        grade_stack.clone(),
    ))
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

    let ffi = encode_grade_stack_draft_recipe_v1(authored.clone());
    assert_eq!(
        ffi.grade_nodes[0]
            .fine
            .oklab_color_warper_control_points
            .len(),
        50
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

    let encoded = encode_grade_stack_draft_recipe_v1(draft);
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
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![grade_node],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
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
fn fine_edit_ffi_validation_rejects_wrong_band_shapes_and_invalid_values() {
    let mut wrong_shape = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    wrong_shape.fine.mixer_hue.pop();
    assert!(
        decode_grade_stack_draft_recipe_v1(&wrong_shape)
            .expect_err("seven hue bands must fail closed")
            .to_string()
            .contains("exactly 8")
    );

    let mut invalid_tone = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_tone.fine.highlights = 1.01;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_tone)
            .expect_err("out-of-range highlights must fail closed")
            .to_string()
            .contains("highlights")
    );

    let mut invalid_global_balance = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_global_balance.fine.global_b_balance = f64::NAN;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_global_balance)
            .expect_err("non-finite global Oklab balance must fail closed")
            .to_string()
            .contains("global Oklab b balance")
    );

    let mut invalid_range = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_range.fine.color_range_enabled = false;
    invalid_range.fine.color_range_width = f64::NAN;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_range)
            .expect_err("disabled ranges still require canonical finite storage")
            .to_string()
            .contains("color range width")
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
#[cfg(any())]
fn incomplete_recipe_shapes_are_rejected_instead_of_upgraded() {
    let without_curve = recipe_without_sharpen(
        &grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("new neutral Recipe"),
    );
    let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&without_curve)
        .expect_err("a Recipe missing the required Detail node must fail closed");
    assert!(!format!("{error:#}").is_empty());

    let curved_draft = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![GradeNodeDraft {
            tone_curve: Some(ToneCurveDraft::SmoothRgb(Box::new(SmoothRgbToneCurve {
                master: vec![
                    ToneCurvePoint { x: 0.0, y: 0.0 },
                    ToneCurvePoint { x: 0.5, y: 0.65 },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ],
                ..SmoothRgbToneCurve::default()
            }))),
            ..GradeNodeDraft::neutral(BASIC_LAYER_LABEL)
        }],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };
    let with_curve = recipe_without_sharpen(
        &grade_stack_recipe_v1_snapshot(&curved_draft, None).expect("new curved Recipe"),
    );
    let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&with_curve)
        .expect_err("an ambiguous incomplete Recipe must fail closed");
    assert!(!format!("{error:#}").is_empty());
}

#[test]
#[cfg(any())]
fn ffi_tone_curve_round_trip_preserves_every_control_point() {
    let incoming = ffi_settings_with_tone(
        0.4,
        1.2,
        [0.05, 0.0],
        0.9,
        &[[0.0, -0.1], [0.2, 0.08], [0.7, 0.82], [1.0, 1.2]],
    );
    let settings = decode_grade_stack_draft_recipe_v1(&incoming).expect("validate FFI Grade Stack");
    let snapshot =
        grade_stack_recipe_v1_snapshot(&settings, None).expect("build Recipe v1 snapshot");
    let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode full Grade Stack");
    let outgoing = encode_grade_stack_draft_recipe_v1(decoded.clone());

    assert_eq!(decoded, settings);
    assert_eq!(outgoing.tone_curve_kind, ffi::FfiToneCurveKind::SmoothRgb);
    assert_eq!(ffi_curve_pairs(&outgoing), ffi_curve_pairs(&incoming));
}

#[test]
#[cfg(any())]
fn smooth_rgb_tone_curve_v2_round_trips_ffi_recipe_and_render_contract() {
    let master = [[0.0, 0.02], [0.45, 0.61], [1.0, 1.0]];
    let red = [[0.0, 0.0], [0.6, 0.7], [1.0, 1.0]];
    let green = [[0.0, 0.0], [1.0, 1.0]];
    let blue = [[0.0, 0.0], [0.3, 0.22], [0.8, 0.9], [1.0, 1.0]];
    let incoming = ffi_settings_with_smooth_tone(&master, &red, &green, &blue);
    let settings = decode_grade_stack_draft_recipe_v1(&incoming).expect("decode smooth RGB v2 FFI");
    let snapshot =
        grade_stack_recipe_v1_snapshot(&settings, None).expect("persist smooth RGB v2 Recipe");
    let nodes =
        single_grade_node_recipe_v1_render_ops(&snapshot).expect("read smooth RGB v2 Recipe nodes");
    let curve_node = nodes.tone_curve.expect("smooth Tone Curve node");
    assert_eq!(
        curve_node.id(),
        settings.recipe_v1_identity.tone_curve_render_op_id
    );
    assert_eq!(
        curve_node.operation().operation_id().as_str(),
        TONE_CURVE_OPERATION_ID
    );
    assert_eq!(
        curve_node.operation().parameter_schema_version(),
        TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
    );
    assert_eq!(
        curve_node.operation().implementation_version(),
        TONE_CURVE_V2_IMPLEMENTATION_VERSION
    );

    let plan = compile_recipe_render_plan(&snapshot).expect("compile smooth RGB v2 Recipe");
    let rendered = plan
        .nodes
        .iter()
        .find_map(|node| match &node.operation {
            AdjustmentRenderOperation::SmoothRgbToneCurve { curves } => Some((node, curves)),
            _ => None,
        })
        .expect("compiled smooth RGB v2 operation");
    assert_eq!(
        rendered.0.parameter_schema_version,
        SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION
    );
    assert_eq!(
        rendered.0.implementation_version,
        SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION
    );
    assert_eq!(
        rendered.1.master,
        master
            .into_iter()
            .map(|[x, y]| ToneCurvePoint { x, y })
            .collect::<Vec<_>>()
    );
    assert_eq!(
        rendered.1.blue,
        blue.into_iter()
            .map(|[x, y]| ToneCurvePoint { x, y })
            .collect::<Vec<_>>()
    );

    let outgoing = encode_grade_stack_draft_recipe_v1(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode persisted smooth RGB v2 Recipe"),
    );
    assert_eq!(outgoing.tone_curve_kind, ffi::FfiToneCurveKind::SmoothRgb);
    assert_eq!(
        ffi_curve_pairs_from(&outgoing.tone_curve_master_points),
        master
    );
    assert_eq!(ffi_curve_pairs_from(&outgoing.tone_curve_red_points), red);
    assert_eq!(
        ffi_curve_pairs_from(&outgoing.tone_curve_green_points),
        green
    );
    assert_eq!(ffi_curve_pairs_from(&outgoing.tone_curve_blue_points), blue);
}

#[test]
#[cfg(any())]
fn current_curve_contract_is_canonical_and_keeps_its_stable_id() {
    let points = [[0.0, 0.03], [0.5, 0.68], [1.0, 1.0]];
    let settings = decode_grade_stack_draft_recipe_v1(&ffi_settings_with_tone(
        0.0, 1.0, [0.0; 2], 1.0, &points,
    ))
    .expect("decode current Tone Curve");
    let snapshot =
        grade_stack_recipe_v1_snapshot(&settings, None).expect("persist current Tone Curve");
    let node = single_grade_node_recipe_v1_render_ops(&snapshot)
        .expect("read current Tone Curve")
        .tone_curve
        .expect("current Tone Curve node");
    assert_eq!(
        node.operation().parameter_schema_version(),
        TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
    );
    assert_eq!(
        node.operation().implementation_version(),
        TONE_CURVE_V2_IMPLEMENTATION_VERSION
    );
    assert!(matches!(
        compile_recipe_render_plan(&snapshot).unwrap().nodes[6].operation,
        AdjustmentRenderOperation::SmoothRgbToneCurve { .. }
    ));

    let rebuilt = grade_stack_recipe_v1_snapshot(
        &decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode current Tone Curve"),
        Some(&snapshot),
    )
    .expect("save unchanged current Tone Curve");
    let rebuilt_node = single_grade_node_recipe_v1_render_ops(&rebuilt)
        .unwrap()
        .tone_curve
        .unwrap();
    assert_eq!(rebuilt_node.id(), node.id());
    assert_eq!(rebuilt_node.parameters(), node.parameters());
}

#[test]
#[cfg(any())]
fn neutral_before_ignores_transient_slider_parameters() {
    let mut non_neutral = ffi_settings_with_tone(
        2.0,
        1.7,
        [0.2, -0.1],
        0.6,
        &[[0.0, 0.1], [0.5, 0.8], [1.0, 1.1]],
    );
    non_neutral.enabled = false;

    let before =
        preview_grade_stack_draft_recipe_v1(&non_neutral, false).expect("select neutral Before");
    assert_eq!(before.grade_nodes.len(), 1);
    assert_eq!(before.basic, BasicEditParameters::default());
    assert!(before.tone_curve.is_none());
    assert!(before.enabled);
    let current =
        preview_grade_stack_draft_recipe_v1(&non_neutral, true).expect("select current parameters");
    assert_ne!(current.basic, BasicEditParameters::default());
    assert!(!current.enabled);
}

#[test]
#[cfg(any())]
fn tone_curve_ffi_validation_rejects_invalid_geometry_without_repair() {
    assert_invalid_curve(&[[0.0, 0.0]], "2 through 256");
    assert_invalid_curve(&[[0.1, 0.0], [1.0, 1.0]], "start at zero");
    assert_invalid_curve(
        &[[0.0, 0.0], [0.5, 0.4], [0.5, 0.7], [1.0, 1.0]],
        "strictly increasing",
    );
    assert_invalid_curve(&[[0.0, 0.0], [1.0, f64::NAN]], "finite values");
    let maximum = u32::try_from(MAX_TONE_CURVE_POINTS).expect("Tone Curve bound fits u32");
    let too_many = (0..=maximum)
        .map(|index| {
            let value = f64::from(index) / f64::from(maximum);
            [value, value]
        })
        .collect::<Vec<_>>();
    assert_invalid_curve(&too_many, "2 through 256");

    let mut inconsistent = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    inconsistent.tone_curve_master_points = vec![
        ffi::FfiToneCurvePoint { x: 0.0, y: 0.0 },
        ffi::FfiToneCurvePoint { x: 1.0, y: 1.0 },
    ];
    let error = decode_grade_stack_draft_recipe_v1(&inconsistent)
        .expect_err("presence flag mismatch must fail");
    assert!(error.to_string().contains("kind is None"));

    let mut smooth_missing_blue = ffi_settings_with_smooth_tone(
        &[[0.0, 0.0], [1.0, 1.0]],
        &[[0.0, 0.0], [1.0, 1.0]],
        &[[0.0, 0.0], [1.0, 1.0]],
        &[[0.0, 0.0], [1.0, 1.0]],
    );
    smooth_missing_blue.tone_curve_blue_points.clear();
    let error = decode_grade_stack_draft_recipe_v1(&smooth_missing_blue)
        .expect_err("the current curve requires explicit points for every channel");
    assert!(format!("{error:#}").contains("blue channel"));
}

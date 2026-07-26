//! Adjustment-plan validation, flattening, color, curve, and retouch contracts.

use super::*;

#[test]
fn basic_edit_defaults_are_a_bounded_neutral_recipe() {
    let request = EditedProxyRequest::default();
    assert_eq!(request.edits, BasicEditParameters::default());
    assert_eq!(request.max_edge, 2_048);
    assert_eq!(request.jpeg_quality, 95);
    request.validate().expect("neutral recipe is valid");
}

#[test]
fn basic_compatibility_builds_the_expected_typed_plan() {
    let plan = basic_adjustment_render_plan(BasicEditParameters::default())
        .expect("neutral basic edits compile");
    assert_eq!(plan.nodes.len(), 4);
    assert!(matches!(
        plan.nodes[0].operation,
        AdjustmentRenderOperation::RgbWhiteBalance {
            temperature: 0.0,
            tint: 0.0
        }
    ));
    assert!(matches!(
        plan.nodes[1].operation,
        AdjustmentRenderOperation::Exposure { stops: 0.0 }
    ));
    assert!(matches!(
        plan.nodes[2].operation,
        AdjustmentRenderOperation::Contrast {
            factor: 1.0,
            pivot: 0.18
        }
    ));
    assert!(matches!(
        plan.nodes[3].operation,
        AdjustmentRenderOperation::Saturation { factor: 1.0 }
    ));
}

#[test]
fn typed_plan_rejects_duplicate_ids_and_malformed_curves() {
    let node = |node_id: &str, operation| AdjustmentRenderNode {
        node_id: node_id.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation,
    };
    let duplicate = AdjustmentRenderPlan {
        nodes: vec![
            node("same", AdjustmentRenderOperation::Exposure { stops: 0.0 }),
            node(
                "same",
                AdjustmentRenderOperation::Saturation { factor: 1.0 },
            ),
        ],
        geometry: AdjustmentGeometry::identity(),
    };
    assert!(matches!(
        duplicate.validate(),
        Err(BridgeError::InvalidEditRequest(_))
    ));

    for points in [
        vec![
            ToneCurvePoint { x: 0.1, y: 0.0 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
        vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.0, y: 0.5 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    ] {
        let malformed = AdjustmentRenderPlan {
            nodes: vec![AdjustmentRenderNode {
                node_id: "curve".to_owned(),
                parameter_schema_version: OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                implementation_version: OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
                enabled: true,
                operation: AdjustmentRenderOperation::OklabLightnessToneCurve {
                    curve: Box::new(OklabLightnessToneCurve { lightness: points }),
                },
            }],
            geometry: AdjustmentGeometry::identity(),
        };
        assert!(matches!(
            malformed.validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }

    for operation in [
        AdjustmentRenderOperation::Exposure { stops: -2_000.0 },
        AdjustmentRenderOperation::Contrast {
            factor: -0.1,
            pivot: 0.18,
        },
        AdjustmentRenderOperation::RgbWhiteBalance {
            temperature: 0.0,
            tint: 2.0,
        },
        AdjustmentRenderOperation::Saturation { factor: -0.1 },
    ] {
        let invalid = AdjustmentRenderPlan {
            nodes: vec![AdjustmentRenderNode {
                node_id: "invalid".to_owned(),
                parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                enabled: true,
                operation,
            }],
            geometry: AdjustmentGeometry::identity(),
        };
        assert!(matches!(
            invalid.validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
}

#[test]
#[allow(clippy::float_cmp)] // FFI flattening is an exact in-memory contract.
fn extended_plan_validates_and_flattens_the_stable_ffi_contract() {
    let perceptual = PerceptualColorParameters {
        global_a_balance: -0.25,
        global_b_balance: 0.4,
        vibrance: 0.2,
        hue_shifts: [0.1; COLOR_MIXER_BAND_COUNT],
        saturation: [-0.2; COLOR_MIXER_BAND_COUNT],
        lightness: [0.3; COLOR_MIXER_BAND_COUNT],
        color_range: ColorRangeParameters {
            enabled: true,
            center_hue_degrees: 45.0,
            width_degrees: 60.0,
            softness: 0.4,
            hue_shift_degrees: 15.0,
            saturation: 0.5,
            lightness: -0.6,
        },
        additional_color_ranges: Vec::new(),
        selective_color_relative: false,
        selective_color_lightness_protection: 0.35,
        selective_color_cmyk: [0.25; SELECTIVE_COLOR_VALUE_COUNT],
    };
    let plan = AdjustmentRenderPlan {
        nodes: vec![
            AdjustmentRenderNode {
                node_id: "selective-tone".to_owned(),
                parameter_schema_version: SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
                implementation_version: SELECTIVE_TONE_IMPLEMENTATION_VERSION,
                enabled: true,
                operation: AdjustmentRenderOperation::SelectiveTone {
                    parameters: SelectiveToneParameters {
                        highlights: -1.0,
                        shadows: -0.25,
                        whites: 0.5,
                        blacks: 1.0,
                    },
                },
            },
            AdjustmentRenderNode {
                node_id: "perceptual-color".to_owned(),
                parameter_schema_version: PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION,
                implementation_version: PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
                enabled: true,
                operation: AdjustmentRenderOperation::PerceptualColor {
                    parameters: Box::new(perceptual),
                },
            },
            AdjustmentRenderNode {
                node_id: "technical-detail".to_owned(),
                parameter_schema_version: TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
                implementation_version: TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
                enabled: true,
                operation: AdjustmentRenderOperation::Sharpen {
                    pass: AdjustmentDetailEffectsPass::TechnicalDetail,
                    parameters: Box::new(SharpenParameters {
                        amount: 1.25,
                        radius: 2.5,
                        threshold: 0.15,
                        masking: 0.75,
                        local_contrast: 0.6,
                        local_contrast_scale: 0.7,
                        ..SharpenParameters::default()
                    }),
                },
            },
        ],
        geometry: AdjustmentGeometry::identity(),
    };

    plan.validate().expect("extended plan is valid");
    let selective_ffi = ffi_render_node(&plan.nodes[0]);
    assert!(matches!(
        selective_ffi.operation,
        ffi::FfiAdjustmentOperation::SelectiveTone
    ));
    assert_eq!(selective_ffi.parameters, [-1.0, -0.25, 0.5, 1.0]);

    let perceptual_ffi = ffi_render_node(&plan.nodes[1]);
    assert!(matches!(
        perceptual_ffi.operation,
        ffi::FfiAdjustmentOperation::PerceptualColor
    ));
    assert_eq!(perceptual_ffi.parameters.len(), 72);
    assert_eq!(perceptual_ffi.parameter_group_lengths, [0]);
    assert_eq!(perceptual_ffi.parameters[0], 0.2);
    assert_eq!(&perceptual_ffi.parameters[1..9], &[0.1; 8]);
    assert_eq!(&perceptual_ffi.parameters[9..17], &[-0.2; 8]);
    assert_eq!(&perceptual_ffi.parameters[17..25], &[0.3; 8]);
    assert_eq!(
        &perceptual_ffi.parameters[25..32],
        &[1.0, 45.0, 60.0, 0.4, 15.0, 0.5, -0.6]
    );
    assert_eq!(perceptual_ffi.parameters[32], 0.0);
    assert_eq!(perceptual_ffi.parameters[33], 0.35);
    assert_eq!(
        &perceptual_ffi.parameters[34..70],
        &[0.25; SELECTIVE_COLOR_VALUE_COUNT]
    );
    assert_eq!(&perceptual_ffi.parameters[70..], &[-0.25, 0.4]);

    let sharpen_ffi = ffi_render_node(&plan.nodes[2]);
    assert!(matches!(
        sharpen_ffi.operation,
        ffi::FfiAdjustmentOperation::Sharpen
    ));
    assert_eq!(sharpen_ffi.parameters.len(), 37);
    assert_eq!(&sharpen_ffi.parameters[..4], &[1.25, 2.5, 0.15, 0.75]);
    assert_eq!(&sharpen_ffi.parameters[4..8], &[0.0, 0.0, 0.6, 0.7]);
    assert_eq!(
        &sharpen_ffi.parameters[8..],
        &[
            0.0, 0.5, 0.0, 0.0, 0.0, 270.0, 340.0, 0.0, 100.0, 165.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.5, 0.5, 0.0, 0.5, 0.0, 0.5, 0.0,
        ]
    );
}

#[test]
#[allow(clippy::float_cmp)] // FFI flattening is an exact in-memory contract.
fn color_warper_plan_validates_and_flattens_fixed_lattice() {
    let mut parameters = OklabColorWarperParameters {
        strength: 0.65,
        ..OklabColorWarperParameters::default()
    };
    parameters.control_points[0] = OklabColorWarperControlPoint {
        a_offset: -0.12,
        b_offset: 0.08,
    };
    parameters.control_points[OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT - 1] =
        OklabColorWarperControlPoint {
            a_offset: 0.15,
            b_offset: -0.06,
        };
    let plan = AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "oklab-color-warper".to_owned(),
            parameter_schema_version: OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
            implementation_version: OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
            enabled: true,
            operation: AdjustmentRenderOperation::OklabColorWarper {
                parameters: Box::new(parameters),
            },
        }],
        geometry: AdjustmentGeometry::identity(),
    };

    plan.validate().expect("Color Warper lattice is valid");
    let flattened = ffi_render_node(&plan.nodes[0]);
    assert!(matches!(
        flattened.operation,
        ffi::FfiAdjustmentOperation::OklabColorWarper
    ));
    assert_eq!(
        flattened.parameters.len(),
        1 + OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2
    );
    assert!(flattened.parameter_group_lengths.is_empty());
    assert!(flattened.payload.is_empty());
    assert_eq!(flattened.parameters[0], 0.65);
    assert_eq!(&flattened.parameters[1..5], &[-0.12, 0.08, 0.0, 0.0]);
    assert_eq!(&flattened.parameters[49..], &[0.15, -0.06]);
}

#[test]
fn color_warper_rejects_invalid_strength_and_control_offsets() {
    let plan = |parameters| AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "invalid-oklab-color-warper".to_owned(),
            parameter_schema_version: OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
            implementation_version: OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
            enabled: true,
            operation: AdjustmentRenderOperation::OklabColorWarper {
                parameters: Box::new(parameters),
            },
        }],
        geometry: AdjustmentGeometry::identity(),
    };
    let invalid_strength = OklabColorWarperParameters {
        strength: 1.01,
        ..OklabColorWarperParameters::default()
    };
    let mut invalid_offset = OklabColorWarperParameters::default();
    invalid_offset.control_points[7].b_offset = OKLAB_COLOR_WARPER_MAXIMUM_OFFSET + 0.001;
    let mut non_finite_offset = OklabColorWarperParameters::default();
    non_finite_offset.control_points[11].a_offset = f64::NAN;
    for parameters in [invalid_strength, invalid_offset, non_finite_offset] {
        assert!(matches!(
            plan(parameters).validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
}

#[test]
fn extended_plan_rejects_non_finite_and_out_of_range_values() {
    let node = |operation| AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "invalid-extended-control".to_owned(),
            parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
            implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
            enabled: true,
            operation,
        }],
        geometry: AdjustmentGeometry::identity(),
    };

    for parameters in [
        SelectiveToneParameters {
            highlights: 1.01,
            ..SelectiveToneParameters::default()
        },
        SelectiveToneParameters {
            shadows: f64::NAN,
            ..SelectiveToneParameters::default()
        },
    ] {
        assert!(matches!(
            node(AdjustmentRenderOperation::SelectiveTone { parameters }).validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }

    let mut invalid_mixer = PerceptualColorParameters::default();
    invalid_mixer.hue_shifts[3] = -1.01;
    let mut invalid_global_balance = PerceptualColorParameters::default();
    invalid_global_balance.global_a_balance = 1.01;
    let mut invalid_disabled_range = PerceptualColorParameters::default();
    invalid_disabled_range.color_range.enabled = false;
    invalid_disabled_range.color_range.width_degrees = 0.0;
    for parameters in [
        invalid_mixer,
        invalid_global_balance,
        invalid_disabled_range,
    ] {
        assert!(matches!(
            node(AdjustmentRenderOperation::PerceptualColor {
                parameters: Box::new(parameters),
            })
            .validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }

    for parameters in [
        SharpenParameters {
            amount: 2.01,
            ..SharpenParameters::default()
        },
        SharpenParameters {
            radius: 0.09,
            ..SharpenParameters::default()
        },
        SharpenParameters {
            threshold: 1.01,
            ..SharpenParameters::default()
        },
        SharpenParameters {
            masking: f64::NAN,
            ..SharpenParameters::default()
        },
        SharpenParameters {
            local_contrast: 1.01,
            ..SharpenParameters::default()
        },
        SharpenParameters {
            local_contrast_scale: -0.01,
            ..SharpenParameters::default()
        },
    ] {
        assert!(matches!(
            node(AdjustmentRenderOperation::Sharpen {
                pass: AdjustmentDetailEffectsPass::TechnicalDetail,
                parameters: Box::new(parameters),
            })
            .validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
}

#[test]
fn typed_plan_rejects_unsupported_contract_versions() {
    for (parameter_schema_version, implementation_version) in [
        (
            ADJUSTMENT_PARAMETER_SCHEMA_VERSION + 1,
            ADJUSTMENT_IMPLEMENTATION_VERSION,
        ),
        (
            ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
            ADJUSTMENT_IMPLEMENTATION_VERSION + 1,
        ),
    ] {
        let plan = AdjustmentRenderPlan {
            nodes: vec![AdjustmentRenderNode {
                node_id: "future-exposure".to_owned(),
                parameter_schema_version,
                implementation_version,
                enabled: true,
                operation: AdjustmentRenderOperation::Exposure { stops: 0.0 },
            }],
            geometry: AdjustmentGeometry::identity(),
        };

        assert!(matches!(
            plan.validate(),
            Err(BridgeError::InvalidEditRequest(
                "adjustment node uses an unsupported schema or implementation version"
            ))
        ));
    }

    let one_pass_v2 = AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "discarded-selective-tone-v2".to_owned(),
            parameter_schema_version: 2,
            implementation_version: 2,
            enabled: true,
            operation: AdjustmentRenderOperation::SelectiveTone {
                parameters: SelectiveToneParameters {
                    shadows: 0.5,
                    ..SelectiveToneParameters::default()
                },
            },
        }],
        geometry: AdjustmentGeometry::identity(),
    };
    assert!(matches!(
        one_pass_v2.validate(),
        Err(BridgeError::InvalidEditRequest(
            "adjustment node uses an unsupported schema or implementation version"
        ))
    ));
}

#[test]
fn continuous_retouch_strokes_validate_and_flatten_with_their_point_groups() {
    let node = AdjustmentRenderNode {
        node_id: "continuous-retouch".to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::SpotHeal {
            targets: Vec::new(),
            strokes: vec![AdjustmentRetouchStroke {
                points: vec![
                    AdjustmentRetouchStrokePoint { x: 0.2, y: 0.3 },
                    AdjustmentRetouchStrokePoint { x: 0.7, y: 0.6 },
                ],
                radius_level_zero_pixels: 24,
                mode: 1,
                source_offset_x_radii: 1.25,
                source_offset_y_radii: -0.75,
                feather: 0.4,
            }],
        },
    };
    validate_render_operation(&node.operation).expect("a bounded continuous clone stroke is valid");

    let flattened = ffi_render_node(&node);
    assert!(matches!(
        flattened.operation,
        ffi::FfiAdjustmentOperation::SpotHeal
    ));
    assert_eq!(flattened.parameter_group_lengths, [0, 1, 2]);
    assert_eq!(
        flattened.parameters,
        [24.0, 1.0, 1.25, -0.75, 0.4, 0.2, 0.3, 0.7, 0.6]
    );

    let invalid = AdjustmentRenderOperation::SpotHeal {
        targets: Vec::new(),
        strokes: vec![AdjustmentRetouchStroke {
            points: Vec::new(),
            radius_level_zero_pixels: 24,
            mode: 0,
            source_offset_x_radii: 0.0,
            source_offset_y_radii: 0.0,
            feather: 0.28,
        }],
    };
    assert!(validate_render_operation(&invalid).is_err());
}

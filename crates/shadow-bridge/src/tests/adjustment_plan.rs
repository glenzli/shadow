//! Aggregate adjustment-plan, basic compatibility, curve, detail, and retouch contracts.

use crate::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
    AdjustmentDetailEffectsPass, AdjustmentGeometry, AdjustmentRenderNode,
    AdjustmentRenderOperation, AdjustmentRenderPlan, AdjustmentRetouchStroke,
    AdjustmentRetouchStrokePoint, AdjustmentSpotHealTarget, BasicEditParameters, BridgeError,
    EditedProxyRequest, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION, OklabLightnessToneCurve,
    SELECTIVE_TONE_IMPLEMENTATION_VERSION, SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
    SelectiveToneParameters, SharpenParameters, TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
    TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION, ToneCurvePoint,
    adjustment::validate_render_operation, basic_adjustment_render_plan, ffi,
    render_wire::ffi_render_node,
};

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
        liquify: None,
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
            liquify: None,
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
            liquify: None,
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
fn selective_tone_and_detail_flatten_the_stable_ffi_contract() {
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
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    };

    plan.validate().expect("extended plan is valid");
    let selective_ffi = ffi_render_node(&plan.nodes[0]);
    assert!(matches!(
        selective_ffi.operation,
        ffi::FfiAdjustmentOperation::SelectiveTone
    ));
    assert_eq!(selective_ffi.parameters, [-1.0, -0.25, 0.5, 1.0]);

    let sharpen_ffi = ffi_render_node(&plan.nodes[1]);
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
fn selective_tone_and_detail_reject_non_finite_and_out_of_range_values() {
    let node = |operation| AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "invalid-extended-control".to_owned(),
            parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
            implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
            enabled: true,
            operation,
        }],
        liquify: None,
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
            liquify: None,
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
        liquify: None,
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
                source_offset_x_radii: 18.5,
                source_offset_y_radii: -0.75,
                source_rotation_degrees: 30.0,
                source_scale: 1.25,
                source_flip_horizontal: true,
                source_flip_vertical: false,
                feather: 0.4,
                strength: 0.65,
            }],
        },
    };
    validate_render_operation(&node.operation)
        .expect("an image-bounded continuous clone stroke is valid");

    let flattened = ffi_render_node(&node);
    assert!(matches!(
        flattened.operation,
        ffi::FfiAdjustmentOperation::SpotHeal
    ));
    assert_eq!(flattened.parameter_group_lengths, [0, 1, 2]);
    assert_eq!(
        flattened.parameters,
        [
            24.0, 1.0, 18.5, -0.75, 30.0, 1.25, 1.0, 0.0, 0.4, 0.65, 0.2, 0.3, 0.7, 0.6
        ]
    );

    let invalid = AdjustmentRenderOperation::SpotHeal {
        targets: Vec::new(),
        strokes: vec![AdjustmentRetouchStroke {
            points: Vec::new(),
            radius_level_zero_pixels: 24,
            mode: 0,
            source_offset_x_radii: 0.0,
            source_offset_y_radii: 0.0,
            source_rotation_degrees: 0.0,
            source_scale: 1.0,
            source_flip_horizontal: false,
            source_flip_vertical: false,
            feather: 0.28,
            strength: 1.0,
        }],
    };
    assert!(validate_render_operation(&invalid).is_err());

    let invalid_strength = AdjustmentRenderOperation::SpotHeal {
        targets: Vec::new(),
        strokes: vec![AdjustmentRetouchStroke {
            points: vec![AdjustmentRetouchStrokePoint { x: 0.5, y: 0.5 }],
            radius_level_zero_pixels: 24,
            mode: 0,
            source_offset_x_radii: 0.0,
            source_offset_y_radii: 0.0,
            source_rotation_degrees: 0.0,
            source_scale: 1.0,
            source_flip_horizontal: false,
            source_flip_vertical: false,
            feather: 0.28,
            strength: 1.01,
        }],
    };
    assert!(validate_render_operation(&invalid_strength).is_err());
}

#[test]
fn structure_preserving_heal_mode_validates_and_flattens_without_schema_expansion() {
    let node = AdjustmentRenderNode {
        node_id: "structure-heal".to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::SpotHeal {
            targets: vec![AdjustmentSpotHealTarget {
                center_x: 0.5,
                center_y: 0.5,
                radius_level_zero_pixels: 18,
                mode: 2,
                source_offset_x_radii: 3.0,
                source_offset_y_radii: -1.5,
                source_rotation_degrees: 0.0,
                source_scale: 1.0,
                source_flip_horizontal: false,
                source_flip_vertical: false,
                feather: 0.28,
                strength: 1.0,
            }],
            strokes: Vec::new(),
        },
    };
    validate_render_operation(&node.operation).expect("structure-preserving Heal is valid");
    let flattened = ffi_render_node(&node);
    assert_eq!(flattened.parameter_group_lengths, [1, 0]);
    assert!((flattened.parameters[3] - 2.0).abs() < f64::EPSILON);

    let mut invalid = node;
    let AdjustmentRenderOperation::SpotHeal { targets, .. } = &mut invalid.operation else {
        unreachable!("test operation is retouch")
    };
    targets[0].mode = 3;
    assert!(validate_render_operation(&invalid.operation).is_err());
}

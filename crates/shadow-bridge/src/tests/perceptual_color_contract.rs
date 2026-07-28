//! Perceptual Color validation and stable wire-flattening contracts.

use crate::{
    AdjustmentGeometry, AdjustmentRenderNode, AdjustmentRenderOperation, AdjustmentRenderPlan,
    BridgeError, COLOR_MIXER_BAND_COUNT, ColorRangeParameters,
    PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION, PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION,
    PerceptualColorParameters, SELECTIVE_COLOR_VALUE_COUNT, ffi, render_wire::ffi_render_node,
};

fn perceptual_plan(parameters: PerceptualColorParameters, enabled: bool) -> AdjustmentRenderPlan {
    AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "perceptual-color".to_owned(),
            parameter_schema_version: PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION,
            implementation_version: PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
            enabled,
            operation: AdjustmentRenderOperation::PerceptualColor {
                parameters: Box::new(parameters),
            },
        }],
        geometry: AdjustmentGeometry::identity(),
    }
}

#[test]
#[allow(clippy::float_cmp)] // FFI flattening is an exact in-memory contract.
fn perceptual_color_plan_validates_and_flattens_stable_ffi_contract() {
    let parameters = PerceptualColorParameters {
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
    let plan = perceptual_plan(parameters, true);
    plan.validate().expect("Perceptual Color plan is valid");

    let flattened = ffi_render_node(&plan.nodes[0]);
    assert!(matches!(
        flattened.operation,
        ffi::FfiAdjustmentOperation::PerceptualColor
    ));
    assert_eq!(flattened.parameters.len(), 72);
    assert_eq!(flattened.parameter_group_lengths, [0]);
    assert_eq!(flattened.parameters[0], 0.2);
    assert_eq!(&flattened.parameters[1..9], &[0.1; 8]);
    assert_eq!(&flattened.parameters[9..17], &[-0.2; 8]);
    assert_eq!(&flattened.parameters[17..25], &[0.3; 8]);
    assert_eq!(
        &flattened.parameters[25..32],
        &[1.0, 45.0, 60.0, 0.4, 15.0, 0.5, -0.6]
    );
    assert_eq!(flattened.parameters[32], 0.0);
    assert_eq!(flattened.parameters[33], 0.35);
    assert_eq!(
        &flattened.parameters[34..70],
        &[0.25; SELECTIVE_COLOR_VALUE_COUNT]
    );
    assert_eq!(&flattened.parameters[70..], &[-0.25, 0.4]);
}

#[test]
fn perceptual_color_rejects_noncanonical_controls_before_wire_conversion() {
    let mut invalid_mixer = PerceptualColorParameters::default();
    invalid_mixer.hue_shifts[3] = -1.01;
    let mut invalid_global_balance = PerceptualColorParameters::default();
    invalid_global_balance.global_a_balance = 1.01;
    let mut invalid_disabled_range = PerceptualColorParameters::default();
    invalid_disabled_range.color_range.enabled = false;
    invalid_disabled_range.color_range.width_degrees = 0.0;
    let mut invalid_lightness_protection = PerceptualColorParameters::default();
    invalid_lightness_protection.selective_color_lightness_protection = 1.01;
    for parameters in [
        invalid_mixer,
        invalid_global_balance,
        invalid_disabled_range,
        invalid_lightness_protection,
    ] {
        assert!(matches!(
            perceptual_plan(parameters, true).validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }

    let mut out_of_range_cmyk = PerceptualColorParameters::default();
    out_of_range_cmyk.selective_color_cmyk[17] = 1.01;
    assert!(matches!(
        perceptual_plan(out_of_range_cmyk, true).validate(),
        Err(BridgeError::InvalidEditRequest(
            "Selective Color CMYK values must be in -1..=1"
        ))
    ));

    let mut non_finite_cmyk = PerceptualColorParameters::default();
    non_finite_cmyk.selective_color_cmyk[17] = f64::NAN;
    assert!(matches!(
        perceptual_plan(non_finite_cmyk, false).validate(),
        Err(BridgeError::InvalidEditRequest(
            "adjustment render parameters must be finite"
        ))
    ));
}

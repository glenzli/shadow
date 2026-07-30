//! Oklab Color Warper plan validation and fixed-lattice wire contracts.

use crate::{
    AdjustmentGeometry, AdjustmentRenderNode, AdjustmentRenderOperation, AdjustmentRenderPlan,
    BridgeError, OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT, OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
    OKLAB_COLOR_WARPER_MAXIMUM_OFFSET, OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
    OklabColorWarperControlPoint, OklabColorWarperParameters, ffi, render_wire::ffi_render_node,
};

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
        liquify: None,
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
        liquify: None,
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

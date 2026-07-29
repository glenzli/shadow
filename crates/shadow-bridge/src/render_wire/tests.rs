use crate::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentLocalMask,
    AdjustmentMaskBrushPoint, AdjustmentRenderNode, AdjustmentRenderOperation,
};

use super::ffi_render_node;

fn layer_start(mask: Option<AdjustmentLocalMask>) -> AdjustmentRenderNode {
    AdjustmentRenderNode {
        node_id: "mask".to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerStart {
            opacity: 0.75,
            mask,
        },
    }
}

#[test]
#[allow(clippy::float_cmp)] // The flat CXX wire is an exact in-memory protocol.
fn legacy_local_mask_wire_records_remain_exact() {
    let linear = ffi_render_node(&layer_start(Some(AdjustmentLocalMask::LinearGradient {
        start_x: 0.1,
        start_y: 0.2,
        end_x: 0.8,
        end_y: 0.9,
        invert: true,
    })));
    assert_eq!(
        linear.parameters,
        [0.75, 1.0, 0.1, 0.2, 0.8, 0.9, 0.0, 0.0, 0.0, 1.0]
    );
    assert!(linear.parameter_group_lengths.is_empty());

    let radial = ffi_render_node(&layer_start(Some(AdjustmentLocalMask::RadialGradient {
        center_x: 0.4,
        center_y: 0.6,
        radius_x: 0.2,
        radius_y: 0.3,
        feather: 0.5,
        invert: false,
    })));
    assert_eq!(
        radial.parameters,
        [0.75, 2.0, 0.4, 0.6, 0.0, 0.0, 0.2, 0.3, 0.5, 0.0]
    );
    assert!(radial.parameter_group_lengths.is_empty());

    let brush = ffi_render_node(&layer_start(Some(AdjustmentLocalMask::Brush {
        points: vec![AdjustmentMaskBrushPoint {
            x: 0.25,
            y: 0.75,
            begins_stroke: true,
        }],
        radius: 0.04,
        feather: 0.6,
        invert: true,
    })));
    assert_eq!(
        brush.parameters,
        [
            0.75, 3.0, 0.0, 0.0, 0.0, 0.0, 0.04, 0.0, 0.6, 1.0, 0.25, 0.75, 1.0
        ]
    );
    assert_eq!(brush.parameter_group_lengths, [1]);
}

#[test]
#[allow(clippy::float_cmp)] // The new kind numbers and slots are the native handoff contract.
fn condition_masks_use_fixed_kind_four_and_five_wire_records() {
    let luminance = ffi_render_node(&layer_start(Some(AdjustmentLocalMask::LuminanceRange {
        lower: 0.2,
        upper: 0.8,
        softness: 0.15,
        invert: true,
    })));
    assert_eq!(
        luminance.parameters,
        [0.75, 4.0, 0.2, 0.0, 0.8, 0.0, 0.0, 0.0, 0.15, 1.0]
    );
    assert!(luminance.parameter_group_lengths.is_empty());

    let color = ffi_render_node(&layer_start(Some(AdjustmentLocalMask::ColorRange {
        center_hue_degrees: 270.0,
        width_degrees: 45.0,
        softness: 0.4,
        invert: false,
    })));
    assert_eq!(
        color.parameters,
        [0.75, 5.0, 0.75, 0.0, 0.25, 0.0, 0.0, 0.0, 0.4, 0.0]
    );
    assert!(color.parameter_group_lengths.is_empty());
}

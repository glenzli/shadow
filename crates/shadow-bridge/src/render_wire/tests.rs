use crate::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentGeometry,
    AdjustmentLiquify, AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke,
    AdjustmentLiquifyReconstructStroke, AdjustmentLiquifyStroke, AdjustmentLocalMask,
    AdjustmentMaskBrushPoint, AdjustmentRasterMaskEncoding, AdjustmentRenderNode,
    AdjustmentRenderOperation, AdjustmentRenderPlan, EditPreviewMaskCoverageRequest,
};

use super::{ffi_render_node, ffi_render_request, ffi_render_request_with_mask_coverage};

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

#[test]
#[allow(clippy::float_cmp)] // Kind six and its fixed metadata slots are an exact native ABI.
fn managed_raster_uses_kind_six_and_the_immutable_payload_slot() {
    let mask = ffi_render_node(&layer_start(Some(AdjustmentLocalMask::ManagedRaster {
        raster_width: 2,
        raster_height: 2,
        coordinate_width: 6_000,
        coordinate_height: 4_000,
        encoding: AdjustmentRasterMaskEncoding::Gray16Float,
        samples: vec![0x00, 0x00, 0x00, 0x38, 0x00, 0x3c, 0x00, 0x34],
        expansion: -0.35,
        feather: 0.24,
        invert: true,
    })));
    assert_eq!(
        mask.parameters,
        [0.75, 6.0, 2.0, 2.0, 6_000.0, 4_000.0, 2.0, -0.35, 0.24, 1.0]
    );
    assert!(mask.parameter_group_lengths.is_empty());
    assert_eq!(
        mask.payload,
        [0x00, 0x00, 0x00, 0x38, 0x00, 0x3c, 0x00, 0x34]
    );
}

#[test]
fn coverage_target_is_optional_native_input_and_selection_revision_stays_host_side() {
    let plan = AdjustmentRenderPlan {
        nodes: vec![layer_start(None)],
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    };
    let without_coverage = ffi_render_request_with_mask_coverage(&plan, 2_048, 90, None);
    assert!(!without_coverage.mask_coverage_requested);
    assert_eq!(without_coverage.mask_coverage_target_layer_index, 0);

    let with_coverage = ffi_render_request_with_mask_coverage(
        &plan,
        2_048,
        90,
        Some(EditPreviewMaskCoverageRequest {
            target_layer_index: 0,
            mask_selection_revision: u64::MAX,
        }),
    );
    assert!(with_coverage.mask_coverage_requested);
    assert_eq!(with_coverage.mask_coverage_target_layer_index, 0);
}

#[test]
#[allow(clippy::float_cmp)] // This is the exact flat CXX structural-node protocol.
fn liquify_presence_paths_and_stroke_partitions_cross_the_flat_wire_exactly() {
    let plan = AdjustmentRenderPlan {
        nodes: vec![layer_start(None)],
        liquify: Some(AdjustmentLiquify {
            enabled: true,
            strokes: vec![
                AdjustmentLiquifyStroke::Push(AdjustmentLiquifyPushStroke {
                    points: vec![
                        AdjustmentLiquifyPoint {
                            x: 0.1,
                            y: 0.2,
                            pressure: 0.3,
                        },
                        AdjustmentLiquifyPoint {
                            x: 0.4,
                            y: 0.5,
                            pressure: 0.6,
                        },
                    ],
                    radius: 0.07,
                    strength: 0.8,
                    hardness: 0.25,
                }),
                AdjustmentLiquifyStroke::Reconstruct(AdjustmentLiquifyReconstructStroke {
                    points: vec![AdjustmentLiquifyPoint {
                        x: 0.8,
                        y: 0.4,
                        pressure: 0.5,
                    }],
                    radius: 0.12,
                    strength: 0.4,
                    hardness: 0.75,
                }),
            ],
        }),
        geometry: AdjustmentGeometry::identity(),
    };
    plan.validate().expect("bounded Liquify plan");

    let request = ffi_render_request(&plan, 2_048, 90);
    assert!(request.liquify.present);
    assert!(request.liquify.enabled);
    assert_eq!(request.liquify.stroke_kinds, [0, 1]);
    assert_eq!(request.liquify.stroke_point_counts, [2, 1]);
    assert_eq!(
        request.liquify.stroke_parameters,
        [0.07, 0.8, 0.25, 0.12, 0.4, 0.75]
    );
    assert_eq!(request.liquify.points.len(), 3);
    assert_eq!(request.liquify.points[0].x, 0.1);
    assert_eq!(request.liquify.points[0].y, 0.2);
    assert_eq!(request.liquify.points[0].pressure, 0.3);
    assert_eq!(request.liquify.points[2].x, 0.8);
    assert_eq!(request.liquify.points[2].y, 0.4);
    assert_eq!(request.liquify.points[2].pressure, 0.5);
}

#[test]
fn absent_liquify_uses_an_explicit_empty_wire_payload() {
    let plan = AdjustmentRenderPlan {
        nodes: vec![layer_start(None)],
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    };
    let request = ffi_render_request(&plan, 2_048, 90);
    assert!(!request.liquify.present);
    assert!(!request.liquify.enabled);
    assert!(request.liquify.stroke_kinds.is_empty());
    assert!(request.liquify.points.is_empty());
    assert!(request.liquify.stroke_point_counts.is_empty());
    assert!(request.liquify.stroke_parameters.is_empty());
}

use crate::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentGeometry,
    AdjustmentLocalMask, AdjustmentLocalMaskComponent, AdjustmentMaskComponentOperation,
    AdjustmentRenderNode,
};

use super::*;

fn request() -> EditPreviewMaskCoverageRequest {
    EditPreviewMaskCoverageRequest {
        target_layer_index: 1,
        target_component_index: None,
        mask_selection_revision: 41,
    }
}

fn dimensions_2x2() -> ImageDimensions {
    ImageDimensions {
        width: 2,
        height: 2,
    }
}

fn available_coverage() -> ffi::FfiEditPreviewMaskCoverage {
    ffi::FfiEditPreviewMaskCoverage {
        available: true,
        version: EDIT_PREVIEW_MASK_COVERAGE_VERSION.to_owned(),
        layer_index: 1,
        component_selected: false,
        component_index: 0,
        dimensions: ffi::FfiDimensions {
            width: 2,
            height: 2,
        },
        row_stride_bytes: 2,
        samples: vec![0, 64, 192, 255],
    }
}

#[test]
fn exact_current_r8_coverage_round_trips_selection_revision() {
    let validated = validate_mask_coverage(available_coverage(), Some(request()), dimensions_2x2())
        .expect("validate exact paired coverage")
        .expect("coverage available");

    assert_eq!(validated.version, EDIT_PREVIEW_MASK_COVERAGE_VERSION);
    assert_eq!(validated.target_layer_index, 1);
    assert_eq!(validated.mask_selection_revision, 41);
    assert_eq!(validated.dimensions, dimensions_2x2());
    assert_eq!(validated.row_stride_bytes, 2);
    assert_eq!(validated.samples, [0, 64, 192, 255]);
}

#[test]
fn selected_component_identity_round_trips_and_mismatches_fail_closed() {
    let mut selected_request = request();
    selected_request.target_component_index = Some(1);
    let mut selected_coverage = available_coverage();
    selected_coverage.component_selected = true;
    selected_coverage.component_index = 1;
    let validated =
        validate_mask_coverage(selected_coverage, Some(selected_request), dimensions_2x2())
            .expect("validate selected component coverage")
            .expect("selected coverage available");
    assert_eq!(validated.target_component_index, Some(1));

    let mut mismatch = available_coverage();
    mismatch.component_selected = true;
    mismatch.component_index = 0;
    assert!(validate_mask_coverage(mismatch, Some(selected_request), dimensions_2x2()).is_err());
}

#[test]
fn unavailable_coverage_requires_the_complete_empty_sentinel() {
    let empty = ffi::FfiEditPreviewMaskCoverage {
        available: false,
        version: String::new(),
        layer_index: 0,
        component_selected: false,
        component_index: 0,
        dimensions: ffi::FfiDimensions {
            width: 0,
            height: 0,
        },
        row_stride_bytes: 0,
        samples: Vec::new(),
    };
    assert_eq!(
        validate_mask_coverage(empty, Some(request()), dimensions_2x2())
            .expect("accept exact empty sentinel"),
        None
    );

    let mut malformed = available_coverage();
    malformed.available = false;
    assert!(validate_mask_coverage(malformed, Some(request()), dimensions_2x2()).is_err());
}

#[test]
fn coverage_fails_closed_for_unrequested_version_target_geometry_stride_and_length() {
    assert!(validate_mask_coverage(available_coverage(), None, dimensions_2x2()).is_err());

    let mut wrong_version = available_coverage();
    wrong_version.version = "shadow.edit-preview-mask-coverage.v2".to_owned();
    assert!(validate_mask_coverage(wrong_version, Some(request()), dimensions_2x2()).is_err());

    let mut wrong_target = available_coverage();
    wrong_target.layer_index = 0;
    assert!(validate_mask_coverage(wrong_target, Some(request()), dimensions_2x2()).is_err());

    let mut wrong_dimensions = available_coverage();
    wrong_dimensions.dimensions.width = 1;
    assert!(validate_mask_coverage(wrong_dimensions, Some(request()), dimensions_2x2()).is_err());

    let mut padded = available_coverage();
    padded.row_stride_bytes = 3;
    padded.samples = vec![0; 6];
    assert!(validate_mask_coverage(padded, Some(request()), dimensions_2x2()).is_err());

    let mut short = available_coverage();
    short.samples.pop();
    assert!(validate_mask_coverage(short, Some(request()), dimensions_2x2()).is_err());
}

#[test]
fn request_target_must_name_a_compiled_layer_boundary() {
    let plan = AdjustmentRenderPlan {
        nodes: vec![
            layer_start("first"),
            layer_end("first"),
            layer_start("second"),
            layer_end("second"),
        ],
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    };
    assert!(
        validate_mask_coverage_request(
            &plan,
            EditPreviewMaskCoverageRequest {
                target_layer_index: 1,
                target_component_index: None,
                mask_selection_revision: 0,
            }
        )
        .is_ok()
    );
    assert!(
        validate_mask_coverage_request(
            &plan,
            EditPreviewMaskCoverageRequest {
                target_layer_index: 2,
                target_component_index: None,
                mask_selection_revision: 0,
            }
        )
        .is_err()
    );
}

#[test]
fn component_request_requires_an_existing_composite_leaf() {
    let plan = AdjustmentRenderPlan {
        nodes: vec![composite_layer_start(), layer_end("composite")],
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    };
    assert!(
        validate_mask_coverage_request(
            &plan,
            EditPreviewMaskCoverageRequest {
                target_layer_index: 0,
                target_component_index: Some(1),
                mask_selection_revision: 0,
            }
        )
        .is_ok()
    );
    for target_component_index in [Some(2), None] {
        let target_layer_index = if target_component_index.is_some() {
            0
        } else {
            1
        };
        assert!(
            validate_mask_coverage_request(
                &plan,
                EditPreviewMaskCoverageRequest {
                    target_layer_index,
                    target_component_index,
                    mask_selection_revision: 0,
                }
            )
            .is_err()
        );
    }

    let legacy = AdjustmentRenderPlan {
        nodes: vec![layer_start("legacy"), layer_end("legacy")],
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    };
    assert!(
        validate_mask_coverage_request(
            &legacy,
            EditPreviewMaskCoverageRequest {
                target_layer_index: 0,
                target_component_index: Some(0),
                mask_selection_revision: 0,
            }
        )
        .is_err()
    );
}

fn composite_layer_start() -> AdjustmentRenderNode {
    AdjustmentRenderNode {
        node_id: "start-composite".to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerStart {
            opacity: 1.0,
            mask: Some(AdjustmentLocalMask::Composite {
                components: vec![
                    AdjustmentLocalMaskComponent {
                        operation: AdjustmentMaskComponentOperation::Base,
                        enabled: true,
                        mask: AdjustmentLocalMask::LinearGradient {
                            start_x: 0.0,
                            start_y: 0.0,
                            end_x: 1.0,
                            end_y: 1.0,
                            invert: false,
                        },
                    },
                    AdjustmentLocalMaskComponent {
                        operation: AdjustmentMaskComponentOperation::Intersect,
                        enabled: true,
                        mask: AdjustmentLocalMask::RadialGradient {
                            center_x: 0.5,
                            center_y: 0.5,
                            radius_x: 0.4,
                            radius_y: 0.3,
                            feather: 0.5,
                            invert: false,
                        },
                    },
                ],
                invert: false,
            }),
        },
    }
}

fn layer_start(id: &str) -> AdjustmentRenderNode {
    AdjustmentRenderNode {
        node_id: format!("start-{id}"),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerStart {
            opacity: 1.0,
            mask: None,
        },
    }
}

fn layer_end(id: &str) -> AdjustmentRenderNode {
    AdjustmentRenderNode {
        node_id: format!("end-{id}"),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerEnd,
    }
}

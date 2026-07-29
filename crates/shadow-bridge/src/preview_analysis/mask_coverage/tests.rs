use crate::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentGeometry,
    AdjustmentRenderNode,
};

use super::*;

fn request() -> EditPreviewMaskCoverageRequest {
    EditPreviewMaskCoverageRequest {
        target_layer_index: 1,
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
fn unavailable_coverage_requires_the_complete_empty_sentinel() {
    let empty = ffi::FfiEditPreviewMaskCoverage {
        available: false,
        version: String::new(),
        layer_index: 0,
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
        geometry: AdjustmentGeometry::identity(),
    };
    assert!(
        validate_mask_coverage_request(
            &plan,
            EditPreviewMaskCoverageRequest {
                target_layer_index: 1,
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
                mask_selection_revision: 0,
            }
        )
        .is_err()
    );
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

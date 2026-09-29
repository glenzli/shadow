use super::*;
use crate::{AdjustmentRenderNode, AdjustmentRenderOperation};

fn exposure_plan() -> AdjustmentRenderPlan {
    AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "bake/exposure".into(),
            parameter_schema_version: 1,
            implementation_version: 1,
            enabled: true,
            operation: AdjustmentRenderOperation::Exposure { stops: 1.0 },
        }],
        liquify: None,
        geometry: AdjustmentGeometry::default(),
    }
}

#[test]
fn linked_baker_returns_parseable_cube_and_independent_error_measurement() {
    let baked =
        bake_adjustment_lut(&exposure_plan(), 17, &LutBakeCancellation::new().unwrap()).unwrap();
    validate_cube_lut_document(&baked.document).unwrap();
    assert_eq!(baked.probe_count, 4096);
    assert!(baked.maximum_absolute_error < 1e-6);
    assert!(
        std::str::from_utf8(&baked.document)
            .unwrap()
            .contains("linear sRGB")
    );
}

#[test]
fn cancellation_is_shared_and_malformed_resources_are_rejected() {
    let cancellation = LutBakeCancellation::new().unwrap();
    assert!(cancellation.clone().cancel());
    assert!(bake_adjustment_lut(&exposure_plan(), 17, &cancellation).is_err());
    assert!(validate_cube_lut_document(b"LUT_3D_SIZE 2\n0 0 0\n").is_err());
}

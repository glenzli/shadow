use super::{
    AdjustmentLiquify, AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke,
    MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE,
};
use crate::{
    AdjustmentGeometry, AdjustmentRenderNode, AdjustmentRenderOperation, AdjustmentRenderPlan,
};

fn point(x: f64, y: f64, pressure: f64) -> AdjustmentLiquifyPoint {
    AdjustmentLiquifyPoint { x, y, pressure }
}

fn liquify(points: Vec<AdjustmentLiquifyPoint>) -> AdjustmentLiquify {
    AdjustmentLiquify {
        strokes: vec![AdjustmentLiquifyPushStroke {
            points,
            radius: 0.1,
            strength: 0.75,
            hardness: 0.5,
        }],
    }
}

fn plan(liquify: AdjustmentLiquify) -> AdjustmentRenderPlan {
    AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "neutral-exposure".to_owned(),
            parameter_schema_version: 1,
            implementation_version: 1,
            enabled: true,
            operation: AdjustmentRenderOperation::Exposure { stops: 0.0 },
        }],
        liquify: Some(liquify),
        geometry: AdjustmentGeometry::identity(),
    }
}

#[test]
fn bounded_effective_push_path_is_valid() {
    plan(liquify(vec![point(0.25, 0.5, 1.0), point(0.75, 0.5, 0.5)]))
        .validate()
        .expect("bounded authored liquify path is valid");
}

#[test]
fn invalid_or_degenerate_push_paths_fail_before_the_native_bridge() {
    for points in [
        vec![point(0.25, 0.5, 1.0)],
        vec![point(0.25, 0.5, 0.0), point(0.75, 0.5, 0.0)],
        vec![point(0.25, 0.5, 1.0), point(f64::NAN, 0.5, 1.0)],
        vec![point(0.25, 0.5, 1.0); MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE + 1],
    ] {
        assert!(plan(liquify(points)).validate().is_err());
    }
}

#[test]
fn zero_radius_or_strength_is_not_a_persistable_execution_node() {
    let mut value = liquify(vec![point(0.25, 0.5, 1.0), point(0.75, 0.5, 1.0)]);
    value.strokes[0].radius = 0.0;
    assert!(plan(value).validate().is_err());

    let mut value = liquify(vec![point(0.25, 0.5, 1.0), point(0.75, 0.5, 1.0)]);
    value.strokes[0].strength = 0.0;
    assert!(plan(value).validate().is_err());
}

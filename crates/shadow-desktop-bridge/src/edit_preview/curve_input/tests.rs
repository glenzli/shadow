use super::*;
use shadow_bridge::{AdjustmentGeometry, AdjustmentRenderNode, ToneCurvePoint};
fn op(id: impl Into<String>, operation: Op) -> AdjustmentRenderNode {
    AdjustmentRenderNode {
        node_id: id.into(),
        parameter_schema_version: 1,
        implementation_version: 1,
        enabled: true,
        operation,
    }
}
#[test]
fn prefix_excludes_target_and_suffix_but_retains_rgb_master_for_channels() {
    let node = crate::recipe_v1::new_basic_grade_node("Curve").unwrap();
    let id = node.grade_node_id.parse().unwrap();
    let mut curves = RgbToneCurves::default();
    curves.channels[0] = vec![
        ToneCurvePoint { x: 0.0, y: 0.0 },
        ToneCurvePoint { x: 1.0, y: 0.8 },
    ];
    curves.channels[1] = curves.channels[0].clone();
    let plan = AdjustmentRenderPlan {
        geometry: AdjustmentGeometry::default(),
        liquify: None,
        nodes: vec![
            op(
                "start",
                Op::LocalMaskLayerStart {
                    opacity: 0.4,
                    mask: None,
                },
            ),
            op("upstream", Op::Exposure { stops: 0.5 }),
            op(
                format!(
                    "{id}/{}",
                    recipe_v1_oklab_lightness_tone_curve_render_op_id(id)
                ),
                Op::OklabLightnessToneCurve {
                    curve: Box::default(),
                },
            ),
            op(
                format!("{id}/{}", recipe_v1_rgb_tone_curves_render_op_id(id)),
                Op::RgbToneCurves {
                    curves: Box::new(curves.clone()),
                },
            ),
            op(
                format!("{}/{}", node.grade_node_id, node.sharpen_render_op_id),
                Op::Exposure { stops: 1.0 },
            ),
            op("end", Op::LocalMaskLayerEnd),
        ],
    };
    let l = curve_prefix(plan.clone(), &node, 0).unwrap();
    assert_eq!(l.nodes.len(), 3);
    assert!(matches!(
        l.nodes[0].operation,
        Op::LocalMaskLayerStart {
            opacity: 1.0,
            mask: None
        }
    ));
    assert!(matches!(l.nodes[2].operation, Op::LocalMaskLayerEnd));
    assert_eq!(curve_prefix(plan.clone(), &node, 1).unwrap().nodes.len(), 4);
    for channel in 2..=4 {
        let prefix = curve_prefix(plan.clone(), &node, channel).unwrap();
        let Op::RgbToneCurves { curves: retained } = &prefix.nodes[3].operation else {
            panic!("missing master")
        };
        assert_eq!(retained.channels[0], curves.channels[0]);
        assert_eq!(
            retained.channels[1..],
            RgbToneCurves::default().channels[1..]
        );
    }
    assert!(curve_prefix(plan, &node, 5).is_err());
}
#[test]
fn neutral_missing_curves_stop_at_detail_and_disabled_targets_fail() {
    let mut node = crate::recipe_v1::new_basic_grade_node("Curve").unwrap();
    let plan = AdjustmentRenderPlan {
        geometry: AdjustmentGeometry::default(),
        liquify: None,
        nodes: vec![
            op("upstream", Op::Exposure { stops: 0.5 }),
            op(
                format!("{}/{}", node.grade_node_id, node.sharpen_render_op_id),
                Op::Exposure { stops: 1.0 },
            ),
        ],
    };
    for channel in 0..=4 {
        assert_eq!(
            curve_prefix(plan.clone(), &node, channel)
                .unwrap()
                .nodes
                .len(),
            1
        );
    }
    node.enabled = false;
    assert!(curve_prefix(plan, &node, 0).is_err());
}

#[test]
fn real_recipe_compiler_identity_and_layer_boundaries_are_sampleable() {
    use crate::recipe_v1::{
        GradeStackDraft, compile_recipe_render_plan, encode_grade_stack_draft_recipe_v1,
        grade_stack_recipe_v1_snapshot,
    };
    let draft = GradeStackDraft::default();
    let settings = encode_grade_stack_draft_recipe_v1(draft.clone()).unwrap();
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).unwrap();
    let plan = compile_recipe_render_plan(&snapshot).unwrap();
    for channel in 0..=4 {
        let prefix = curve_prefix(plan.clone(), &settings.grade_nodes[0], channel).unwrap();
        assert!(!prefix.nodes.is_empty());
        assert!(prefix.nodes.len() < plan.nodes.len());
    }
}

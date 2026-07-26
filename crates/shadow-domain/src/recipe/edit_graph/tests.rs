use super::*;
use crate::recipe::{FiniteF64, ParameterKey};
use crate::{EntityId, NodeId};

use crate::recipe::test_support::{exposure_graph, node, operation};

#[test]
fn typed_dag_round_trips_through_serde() {
    let graph = exposure_graph();
    let encoded = serde_json::to_string(&graph).expect("serialize graph");
    let decoded: EditGraph = serde_json::from_str(&encoded).expect("deserialize graph");

    decoded
        .validate()
        .expect("round-tripped graph remains valid");
    assert_eq!(graph, decoded);
    assert_eq!(
        decoded.output_type(),
        Some(PortType::Image(ImageDomain::WorkingRgb))
    );
}

#[test]
fn typed_parameters_round_trip_in_canonical_key_order() {
    let parameters = [
        (
            ParameterKey::new("tone.white_balance").expect("key"),
            ParameterValue::FloatVector(vec![
                FiniteF64::new(5_200.0).expect("finite"),
                FiniteF64::new(8.0).expect("finite"),
            ]),
        ),
        (
            ParameterKey::new("tone.exposure_ev").expect("key"),
            ParameterValue::Float(FiniteF64::new(0.35).expect("finite")),
        ),
    ]
    .into_iter()
    .collect::<ParameterBlock>();

    let encoded = serde_json::to_string(&parameters).expect("serialize parameters");
    let decoded: ParameterBlock = serde_json::from_str(&encoded).expect("deserialize parameters");
    assert_eq!(parameters, decoded);
    assert_eq!(
        decoded
            .iter()
            .map(|(key, _)| key.as_str())
            .collect::<Vec<_>>(),
        vec!["tone.exposure_ev", "tone.white_balance"]
    );
}

#[test]
fn dag_rejects_port_type_mismatch() {
    let id = NodeId::new_v7();
    let node = node(
        id,
        operation(
            "shadow.demosaic",
            ProcessingStage::Demosaic,
            vec![PortType::Image(ImageDomain::SensorMosaic)],
            PortType::Image(ImageDomain::SceneLinearRgb),
        ),
        vec![NodeInput::GraphInput { index: 0 }],
    );
    let error = EditGraph::new(
        1,
        vec![PortType::Image(ImageDomain::WorkingRgb)],
        vec![node],
        id,
    )
    .expect_err("mismatched port must fail");

    assert!(matches!(
        error,
        GraphValidationError::PortTypeMismatch { .. }
    ));
}

#[test]
fn dag_rejects_cycles_even_when_node_order_is_reversed() {
    let first = NodeId::new_v7();
    let second = NodeId::new_v7();
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let nodes = vec![
        node(
            first,
            operation(
                "shadow.first",
                ProcessingStage::CreativeColor,
                vec![image],
                image,
            ),
            vec![NodeInput::Node { node_id: second }],
        ),
        node(
            second,
            operation(
                "shadow.second",
                ProcessingStage::CreativeColor,
                vec![image],
                image,
            ),
            vec![NodeInput::Node { node_id: first }],
        ),
    ];

    assert!(matches!(
        EditGraph::new(1, vec![image], nodes, second),
        Err(GraphValidationError::Cycle(_))
    ));
}

#[test]
fn dag_rejects_later_stage_feeding_an_earlier_stage() {
    let later = NodeId::new_v7();
    let earlier = NodeId::new_v7();
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let nodes = vec![
        node(
            later,
            operation("shadow.output", ProcessingStage::Output, vec![image], image),
            vec![NodeInput::GraphInput { index: 0 }],
        ),
        node(
            earlier,
            operation(
                "shadow.exposure",
                ProcessingStage::SceneLinearFoundation,
                vec![image],
                image,
            ),
            vec![NodeInput::Node { node_id: later }],
        ),
    ];

    assert!(matches!(
        EditGraph::new(1, vec![image], nodes, earlier),
        Err(GraphValidationError::StageRegression { .. })
    ));
}

#[test]
fn dag_rejects_nodes_that_do_not_contribute_to_output() {
    let used = NodeId::new_v7();
    let unused = NodeId::new_v7();
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let operation = || {
        operation(
            "shadow.exposure",
            ProcessingStage::SceneLinearFoundation,
            vec![image],
            image,
        )
    };
    let nodes = vec![
        node(used, operation(), vec![NodeInput::GraphInput { index: 0 }]),
        node(
            unused,
            operation(),
            vec![NodeInput::GraphInput { index: 0 }],
        ),
    ];

    assert_eq!(
        EditGraph::new(1, vec![image], nodes, used),
        Err(GraphValidationError::UnreachableNode(unused))
    );
}

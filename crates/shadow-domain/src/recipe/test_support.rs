//! Shared Recipe test builders used by facade and edit-graph contract tests.

use crate::{EntityId, NodeId};

use super::{
    AdjustmentNode, EditGraph, ImageDomain, NodeInput, OperationDescriptor, OperationId,
    ParameterBlock, PortType, ProcessingStage,
};

pub(super) fn operation(
    id: &str,
    stage: ProcessingStage,
    inputs: Vec<PortType>,
    output: PortType,
) -> OperationDescriptor {
    OperationDescriptor::new(
        OperationId::new(id).expect("valid operation id"),
        1,
        "cpu-reference-v1",
        stage,
        inputs,
        output,
        None,
    )
    .expect("valid operation")
}

pub(super) fn node(
    id: NodeId,
    operation: OperationDescriptor,
    inputs: Vec<NodeInput>,
) -> AdjustmentNode {
    AdjustmentNode::new(id, operation, inputs, ParameterBlock::default(), None).expect("valid node")
}

pub(super) fn exposure_graph() -> EditGraph {
    let exposure_id = NodeId::new_v7();
    let curve_id = NodeId::new_v7();
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let exposure = node(
        exposure_id,
        operation(
            "shadow.exposure",
            ProcessingStage::SceneLinearFoundation,
            vec![image],
            image,
        ),
        vec![NodeInput::GraphInput { index: 0 }],
    );
    let curve = node(
        curve_id,
        operation(
            "shadow.curve",
            ProcessingStage::ToneAndLocalContrast,
            vec![image],
            image,
        ),
        vec![NodeInput::Node {
            node_id: exposure_id,
        }],
    );
    EditGraph::new(1, vec![image], vec![curve, exposure], curve_id).expect("valid graph")
}

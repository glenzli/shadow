//! Shared Recipe builders used by facade and responsibility contract tests.

use crate::{EntityId, LayerInstanceId, NodeId};

use super::{
    AdjustmentNode, AdjustmentScope, BlendMode, EditGraph, ImageDomain, LayerContent,
    LayerInstance, NodeInput, OperationDescriptor, OperationId, ParameterBlock, PortType,
    ProcessingStage, UnitInterval,
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

pub(super) fn inline_layer() -> LayerInstance {
    LayerInstance::new(
        LayerInstanceId::new_v7(),
        "Tone foundation",
        AdjustmentScope::Photo,
        LayerContent::Inline {
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect("valid layer")
}

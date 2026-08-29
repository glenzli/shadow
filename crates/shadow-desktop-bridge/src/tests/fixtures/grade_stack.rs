//! Canonical Grade Stack and Recipe graph fixtures.

use std::ops::{Deref, DerefMut};

use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, BASIC_LAYER_LABEL, CPU_REFERENCE_IMPLEMENTATION_VERSION,
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION, EXPOSURE_OPERATION_ID, EXPOSURE_STOPS_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditGraph, EntityId,
    FiniteF64, ImageDomain, LayerContent, LayerInstance, LayerInstanceId, NodeId, NodeInput,
    OperationDescriptor, OperationId, ParameterBlock, ParameterValue, PhotoFoundationNode,
    PortType, ProcessingStage, RecipeSnapshot, UnitInterval,
};
use uuid::Uuid;

use crate::{
    ffi,
    recipe_v1::{
        ffi_photo_foundation_settings, new_basic_grade_node, parameter_block, recipe_v1_render_op,
    },
};

impl Deref for ffi::FfiEditSettings {
    type Target = ffi::FfiGradeNode;

    fn deref(&self) -> &Self::Target {
        self.grade_nodes
            .first()
            .expect("validated FFI edit settings always contain one Grade Node")
    }
}

impl DerefMut for ffi::FfiEditSettings {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.grade_nodes
            .first_mut()
            .expect("validated FFI edit settings always contain one Grade Node")
    }
}

pub(in crate::tests) fn ffi_parameters(
    exposure_stops: f64,
    contrast_factor: f64,
    white_balance: [f64; 2],
    saturation_factor: f64,
) -> ffi::FfiEditSettings {
    let mut grade_node =
        new_basic_grade_node(BASIC_LAYER_LABEL).expect("new Basic test Grade Node");
    // Stable fixture identity keeps tests focused on parameter semantics;
    // identity allocation itself has dedicated UUID coverage.
    let grade_node_id = LayerInstanceId::from_uuid(Uuid::from_u128(1));
    grade_node.grade_node_id = grade_node_id.to_string();
    grade_node.exposure_render_op_id = Uuid::from_u128(2).to_string();
    grade_node.contrast_render_op_id = Uuid::from_u128(3).to_string();
    grade_node.selective_tone_render_op_id = Uuid::from_u128(4).to_string();
    grade_node.white_balance_render_op_id = Uuid::from_u128(5).to_string();
    grade_node.saturation_render_op_id = Uuid::from_u128(6).to_string();
    grade_node.perceptual_color_render_op_id = Uuid::from_u128(7).to_string();
    grade_node.lut_render_op_id = Uuid::from_u128(8).to_string();
    grade_node.sharpen_render_op_id = Uuid::from_u128(9).to_string();
    grade_node.basic = ffi::FfiBasicEditParameters {
        exposure_stops,
        contrast_factor,
        white_balance_temperature: white_balance[0],
        white_balance_tint: white_balance[1],
        saturation_factor,
    };
    ffi::FfiEditSettings {
        foundation: ffi_photo_foundation_settings(
            &PhotoFoundationNode::default(),
            shadow_domain::RawFoundationDenoise::disabled(),
        ),
        grade_nodes: vec![grade_node],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        retouch_enabled: true,
        liquify_enabled: false,
        liquify_strokes: Vec::new(),
        geometry: ffi::FfiPhotoGeometry {
            present: false,
            enabled: true,
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: 0,
            straighten_degrees: 0.0,
            perspective_vertical: 0.0,
            perspective_horizontal: 0.0,
            flip_horizontal: false,
            flip_vertical: false,
        },
    }
}

pub(in crate::tests) fn single_exposure_recipe(
    recipe_schema_version: u32,
    graph_schema_version: u32,
    parameter_schema_version: u32,
    implementation_version: &str,
) -> RecipeSnapshot {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let node_id = NodeId::new_v7();
    let node = AdjustmentNode::new(
        node_id,
        OperationDescriptor::new(
            OperationId::new(EXPOSURE_OPERATION_ID).expect("Exposure operation id"),
            parameter_schema_version,
            implementation_version,
            ProcessingStage::SceneLinearFoundation,
            vec![rgb],
            rgb,
            None,
        )
        .expect("versioned Exposure descriptor"),
        vec![NodeInput::GraphInput { index: 0 }],
        parameter_block([(
            EXPOSURE_STOPS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(0.5).expect("finite Exposure")),
        )])
        .expect("Exposure parameters"),
        None,
    )
    .expect("Exposure node");
    let graph = EditGraph::new(graph_schema_version, vec![rgb], vec![node], node_id)
        .expect("single-node graph");
    one_layer_recipe(recipe_schema_version, graph)
}

pub(in crate::tests) fn branching_merge_recipe() -> RecipeSnapshot {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let left_id = NodeId::new_v7();
    let right_id = NodeId::new_v7();
    let merge_id = NodeId::new_v7();
    let exposure_parameters = || {
        parameter_block([(
            EXPOSURE_STOPS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(0.25).expect("finite Exposure")),
        )])
        .expect("Exposure parameters")
    };
    let left = recipe_v1_render_op(
        left_id,
        EXPOSURE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::GraphInput { index: 0 },
        exposure_parameters(),
    )
    .expect("left branch");
    let right = recipe_v1_render_op(
        right_id,
        EXPOSURE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::GraphInput { index: 0 },
        exposure_parameters(),
    )
    .expect("right branch");
    let merge = AdjustmentNode::new(
        merge_id,
        OperationDescriptor::new(
            OperationId::new("shadow.test_merge").expect("merge operation id"),
            CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
            CPU_REFERENCE_IMPLEMENTATION_VERSION,
            ProcessingStage::CreativeColor,
            vec![rgb, rgb],
            rgb,
            None,
        )
        .expect("merge descriptor"),
        vec![
            NodeInput::Node { node_id: left_id },
            NodeInput::Node { node_id: right_id },
        ],
        ParameterBlock::default(),
        None,
    )
    .expect("merge node");
    let graph = EditGraph::new(
        BASIC_GRAPH_SCHEMA_VERSION,
        vec![rgb],
        vec![left, right, merge],
        merge_id,
    )
    .expect("branching graph");
    one_layer_recipe(CURRENT_RECIPE_SCHEMA_VERSION, graph)
}

fn one_layer_recipe(schema_version: u32, graph: EditGraph) -> RecipeSnapshot {
    RecipeSnapshot::new(
        schema_version,
        vec![
            LayerInstance::new(
                LayerInstanceId::new_v7(),
                BASIC_LAYER_LABEL,
                AdjustmentScope::Photo,
                LayerContent::Inline { graph },
                true,
                UnitInterval::ONE,
                BlendMode::Normal,
                None,
            )
            .expect("single inline layer"),
        ],
    )
    .expect("single-layer Recipe")
}

pub(in crate::tests) fn assert_close(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() < 1.0e-12,
        "expected {expected}, got {actual}"
    );
}

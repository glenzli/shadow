use shadow_domain::{
    AdjustmentNode, EditCommitId, EditEntityEntryV1, EditEntityMapV1, EditGraph, EditObject,
    EditObjectId, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EntityId, ImageDomain, LayerId, LayerRevision, LayerRevisionId,
    LibraryRootV1, NodeId, NodeInput, OperationDescriptor, OperationId, ParameterBlock, PortType,
    ProcessingStage,
};

use crate::EditObjectPackWrite;

pub(super) fn leaf(kind: EditObjectKind, label: &str) -> EditObjectPack {
    let object =
        EditObject::from_canonical_json(kind, 1, &serde_json::json!({ "label": label })).unwrap();
    EditObjectPack::new(object, Vec::new()).unwrap()
}

pub(super) fn shared_grade_revision(
    id: LayerRevisionId,
    layer_id: LayerId,
    revision_number: u32,
    parent: Option<LayerRevisionId>,
    label: &str,
) -> EditObjectPack {
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let node_id = NodeId::new_v7();
    let operation = OperationDescriptor::new(
        OperationId::new("shadow.test.shared-grade").unwrap(),
        1,
        "catalog-test-v1",
        ProcessingStage::CreativeColor,
        vec![image],
        image,
        None,
    )
    .unwrap();
    let node = AdjustmentNode::new(
        node_id,
        operation,
        vec![NodeInput::GraphInput { index: 0 }],
        ParameterBlock::default(),
        None,
    )
    .unwrap();
    let graph = EditGraph::new(1, vec![image], vec![node], node_id).unwrap();
    let revision = LayerRevision::new(id, layer_id, revision_number, parent, label, graph).unwrap();
    let object =
        EditObject::from_canonical_json(EditObjectKind::GradeNodeRevision, 1, &revision).unwrap();
    EditObjectPack::new(object, Vec::new()).unwrap()
}

pub(super) fn initial_pack() -> (EditObjectPackWrite, EditObjectId) {
    let photo = leaf(EditObjectKind::PhotoEditState, "photo-a");
    let map = EditEntityMapV1::new(vec![EditEntityEntryV1 {
        key: "photo/00000000-0000-7000-8000-000000000001".into(),
        value: photo.object().id(),
    }])
    .unwrap()
    .into_object_pack()
    .unwrap();
    let root = LibraryRootV1 {
        photo_recipes: Some(map.object().id()),
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .unwrap();
    let root_id = root.object().id();
    (
        EditObjectPackWrite {
            objects: vec![root, map, photo],
            created_at_ms: 1_721_500_000_000,
        },
        root_id,
    )
}

pub(super) fn commit(
    root: EditObjectId,
    parents: Vec<EditCommitId>,
    at: i64,
) -> EditRepositoryCommit {
    EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root,
        parents,
        message: Some(format!("Library checkpoint {at}")),
        created_at_ms: at,
    })
    .unwrap()
}

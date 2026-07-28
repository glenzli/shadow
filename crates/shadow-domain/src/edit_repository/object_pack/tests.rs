use super::*;
use crate::edit_repository::{content_id::EditObjectId, library_state::EditEntityEntryV1};

fn leaf(label: &str) -> EditObjectPack {
    let object = EditObject::from_canonical_json(
        EditObjectKind::PhotoEditState,
        1,
        &serde_json::json!({ "label": label }),
    )
    .expect("valid leaf");
    EditObjectPack::new(object, Vec::new()).expect("leaf has no edges")
}

#[test]
fn entity_map_object_pack_is_order_independent_and_edges_match_payload() {
    let first = leaf("first");
    let second = leaf("second");
    let left = EditEntityMapV1::new(vec![
        EditEntityEntryV1 {
            key: "photo/b".into(),
            value: second.object().id(),
        },
        EditEntityEntryV1 {
            key: "photo/a".into(),
            value: first.object().id(),
        },
    ])
    .unwrap()
    .into_object_pack()
    .unwrap();
    let right = EditEntityMapV1::new(vec![
        EditEntityEntryV1 {
            key: "photo/a".into(),
            value: first.object().id(),
        },
        EditEntityEntryV1 {
            key: "photo/b".into(),
            value: second.object().id(),
        },
    ])
    .unwrap()
    .into_object_pack()
    .unwrap();
    assert_eq!(left, right);
    assert_eq!(left.edges().len(), 2);
}

#[test]
fn typed_payload_rejects_forged_edges() {
    let map = EditEntityMapV1::new(Vec::new())
        .unwrap()
        .into_object_pack()
        .unwrap();
    let root = LibraryRootV1 {
        photo_recipes: Some(map.object().id()),
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    };
    let object = EditObject::from_canonical_json(EditObjectKind::LibraryRoot, 1, &root).unwrap();
    assert!(matches!(
        EditObjectPack::new(object, Vec::new()),
        Err(EditRepositoryError::ObjectEdgesDoNotMatchPayload)
    ));
}

#[test]
fn library_root_edges_follow_the_stable_role_contract() {
    let photo_recipes = EditObjectId::from_bytes([1; 32]);
    let styles = EditObjectId::from_bytes([2; 32]);
    let pack = LibraryRootV1 {
        photo_recipes: Some(photo_recipes),
        shared_grade_heads: None,
        masks: None,
        styles: Some(styles),
        output_states: None,
    }
    .into_object_pack()
    .unwrap();

    assert_eq!(
        pack.edges()
            .iter()
            .map(|edge| (edge.role(), edge.position(), edge.target()))
            .collect::<Vec<_>>(),
        vec![("photo_recipes", 0, photo_recipes), ("styles", 0, styles),]
    );
    assert_eq!(
        LibraryRootV1::from_object(pack.object()).unwrap(),
        LibraryRootV1 {
            photo_recipes: Some(photo_recipes),
            shared_grade_heads: None,
            masks: None,
            styles: Some(styles),
            output_states: None,
        }
    );
}

#[test]
fn object_pack_sorts_a_valid_typed_edge_index() {
    let photo_recipes = EditObjectId::from_bytes([9; 32]);
    let styles = EditObjectId::from_bytes([8; 32]);
    let object = EditObject::from_canonical_json(
        EditObjectKind::LibraryRoot,
        1,
        &LibraryRootV1 {
            photo_recipes: Some(photo_recipes),
            shared_grade_heads: None,
            masks: None,
            styles: Some(styles),
            output_states: None,
        },
    )
    .unwrap();
    let pack = EditObjectPack::new(
        object,
        vec![
            EditObjectEdge::new("styles", 0, styles).unwrap(),
            EditObjectEdge::new("photo_recipes", 0, photo_recipes).unwrap(),
        ],
    )
    .unwrap();

    assert_eq!(
        pack.edges()
            .iter()
            .map(|edge| (edge.role(), edge.position()))
            .collect::<Vec<_>>(),
        vec![("photo_recipes", 0), ("styles", 0)]
    );
}

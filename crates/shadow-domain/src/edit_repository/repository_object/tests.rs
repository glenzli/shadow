//! Repository object identity, canonicalization, and individual-edge contracts.

use super::*;

#[test]
fn object_ids_are_domain_separated_and_serde_as_hex() {
    let photo = EditObject::from_canonical_json(
        EditObjectKind::PhotoEditState,
        1,
        &serde_json::json!({ "value": 1 }),
    )
    .expect("photo object");
    let style = EditObject::from_canonical_json(
        EditObjectKind::StyleRevision,
        1,
        &serde_json::json!({ "value": 1 }),
    )
    .expect("style object");
    assert_eq!(
        photo.id().to_string(),
        "0b356348baf91cfdb79bf38687ac2cf13d7d301aecf36407cdf99a46a06c71a1"
    );
    assert_ne!(photo.id(), style.id());
    let encoded = serde_json::to_string(&photo.id()).expect("serialize id");
    assert_eq!(encoded.len(), 66);
    assert_eq!(
        serde_json::from_str::<EditObjectId>(&encoded).unwrap(),
        photo.id()
    );
}

#[test]
fn canonical_object_ids_ignore_source_map_iteration_order() {
    let left = std::collections::HashMap::from([("b", 2), ("a", 1)]);
    let right = std::collections::HashMap::from([("a", 1), ("b", 2)]);
    let left = EditObject::from_canonical_json(EditObjectKind::OutputState, 1, &left).unwrap();
    let right = EditObject::from_canonical_json(EditObjectKind::OutputState, 1, &right).unwrap();
    assert_eq!(left, right);
}

#[test]
fn stored_objects_reject_noncanonical_json_and_unknown_versions() {
    let bytes = br#"{"b":2,"a":1}"#.to_vec();
    let id = object_id(EditObjectKind::OutputState.as_str(), 1, &bytes);
    assert!(matches!(
        EditObject::from_stored_parts(id, EditObjectKind::OutputState, 1, bytes),
        Err(EditRepositoryError::NonCanonicalObjectPayload)
    ));
    assert!(matches!(
        EditObject::from_canonical_json(EditObjectKind::OutputState, 2, &serde_json::json!({})),
        Err(EditRepositoryError::UnsupportedObjectFormatVersion {
            format_version: 2,
            ..
        })
    ));
}

#[test]
fn object_edges_validate_and_expose_their_stable_identity() {
    let target = EditObjectId::from_bytes([9; 32]);
    assert!(matches!(
        EditObjectEdge::new("Invalid Role", 0, target),
        Err(EditRepositoryError::InvalidEdgeRole(role)) if role == "Invalid Role"
    ));
    let edge = EditObjectEdge::new("photo_recipe", 3, target).unwrap();
    assert_eq!(edge.role(), "photo_recipe");
    assert_eq!(edge.position(), 3);
    assert_eq!(edge.target(), target);
}

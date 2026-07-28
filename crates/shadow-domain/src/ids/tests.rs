use super::*;

#[test]
fn typed_ids_round_trip_without_losing_their_type() {
    let original = PhotoId::new_v7();
    let text = original.to_string();
    let parsed: PhotoId = text.parse().expect("valid photo id");

    assert_eq!(original, parsed);
}

#[test]
fn typed_ids_serialize_as_uuid_strings() {
    let id = RepresentationId::new_v7();
    let json = serde_json::to_string(&id).expect("serialize representation id");
    let decoded: RepresentationId =
        serde_json::from_str(&json).expect("deserialize representation id");

    assert_eq!(id, decoded);
}

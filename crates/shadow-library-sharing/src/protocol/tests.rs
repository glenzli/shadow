use shadow_domain::{EntityId, PhotoId, RepresentationId};

use super::{LIBRARY_PROTOCOL_VERSION, Request, RequestEnvelope};

#[test]
fn request_round_trip_preserves_typed_remote_identity() {
    let request = RequestEnvelope {
        protocol_version: LIBRARY_PROTOCOL_VERSION,
        authorization: "01234567890123456789012345678901".to_owned(),
        request: Request::PrepareOriginal {
            photo_id: PhotoId::new_v7(),
            representation_id: RepresentationId::new_v7(),
        },
    };
    let json = serde_json::to_vec(&request).expect("serialize request");
    let decoded: RequestEnvelope = serde_json::from_slice(&json).expect("deserialize request");
    assert_eq!(decoded.protocol_version, LIBRARY_PROTOCOL_VERSION);
    assert!(matches!(decoded.request, Request::PrepareOriginal { .. }));
}

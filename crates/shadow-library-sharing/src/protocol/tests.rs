use shadow_domain::{EntityId, PhotoId, RepresentationId, RepresentationKind};

use super::{
    LIBRARY_PROTOCOL_REVISION, PreviewUnavailableReason, RemoteOriginalIdentity,
    RemotePhotoManifest, RemotePhotoMetadata, RemotePreviewAvailability,
    RemoteRepresentationManifest, Request, RequestEnvelope,
};

#[test]
fn request_round_trip_preserves_typed_remote_identity() {
    let request = RequestEnvelope {
        protocol_revision: LIBRARY_PROTOCOL_REVISION,
        authorization: "01234567890123456789012345678901".to_owned(),
        request: Request::PrepareOriginal {
            photo_id: PhotoId::new_v7(),
            representation_id: RepresentationId::new_v7(),
        },
    };
    let json = serde_json::to_vec(&request).expect("serialize request");
    assert_eq!(LIBRARY_PROTOCOL_REVISION, 2_026_080_601);
    assert!(String::from_utf8_lossy(&json).contains("\"protocol_revision\":2026080601"));
    let decoded: RequestEnvelope = serde_json::from_slice(&json).expect("deserialize request");
    assert_eq!(decoded.protocol_revision, LIBRARY_PROTOCOL_REVISION);
    assert!(matches!(decoded.request, Request::PrepareOriginal { .. }));
}

#[test]
fn logical_photo_manifest_exposes_the_preferred_original_identity() {
    let photo_id = PhotoId::new_v7();
    let raw_id = RepresentationId::new_v7();
    let jpeg_id = RepresentationId::new_v7();
    let manifest = RemotePhotoManifest {
        photo_id,
        representation_id: raw_id,
        display_name: "IMG_0001.NEF".to_owned(),
        source_byte_len: 42,
        source_modified_at_ms: Some(7),
        metadata: RemotePhotoMetadata::default(),
        preview: RemotePreviewAvailability::Unavailable {
            reason: PreviewUnavailableReason::NotPrepared,
        },
        representations: vec![
            RemoteRepresentationManifest {
                representation_id: raw_id,
                kind: RepresentationKind::OriginalRaw,
                display_name: "IMG_0001.NEF".to_owned(),
                source_byte_len: 42,
                source_modified_at_ms: Some(7),
                location_count: 2,
                online_location_count: 1,
                original_identity: RemoteOriginalIdentity::Available {
                    digest_blake3: [9; 32],
                },
            },
            RemoteRepresentationManifest {
                representation_id: jpeg_id,
                kind: RepresentationKind::OriginalRaster,
                display_name: "IMG_0001.JPG".to_owned(),
                source_byte_len: 12,
                source_modified_at_ms: Some(7),
                location_count: 1,
                online_location_count: 1,
                original_identity: RemoteOriginalIdentity::NotPrepared,
            },
        ],
    };

    assert_eq!(manifest.preferred_original_digest(), Some([9; 32]));
    let encoded = serde_json::to_vec(&manifest).expect("encode logical photo manifest");
    let decoded: RemotePhotoManifest =
        serde_json::from_slice(&encoded).expect("decode logical photo manifest");
    assert_eq!(decoded, manifest);
}

use shadow_domain::{
    EntityId, GpsMetadataSnapshot, ImageDimensions, PhotoId, RepresentationId, RepresentationKind,
};

use super::{
    LIBRARY_PROTOCOL_REVISION, PreviewUnavailableReason, REMOTE_PHOTO_METADATA_SCHEMA_VERSION,
    RemoteOriginalIdentity, RemotePhotoManifest, RemotePhotoMetadata, RemotePreviewAvailability,
    RemotePreviewManifest, RemotePreviewPixelOrientation, RemoteRepresentationManifest, Request,
    RequestEnvelope,
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
fn provider_neutral_metadata_round_trip_preserves_orientation_location_and_dimensions() {
    let metadata = RemotePhotoMetadata {
        orientation: Some(6),
        gps: Some(GpsMetadataSnapshot {
            latitude_degrees: 31.2304,
            longitude_degrees: 121.4737,
            altitude_meters: Some(18.5),
        }),
        image_dimensions: Some(ImageDimensions {
            width: 5_504,
            height: 8_256,
        }),
        focal_length_35mm: Some(52.0),
        ..RemotePhotoMetadata::default()
    };

    let encoded = serde_json::to_vec(&metadata).expect("encode metadata");
    let decoded: RemotePhotoMetadata = serde_json::from_slice(&encoded).expect("decode metadata");
    assert_eq!(decoded, metadata);
    assert_eq!(decoded.schema_version, REMOTE_PHOTO_METADATA_SCHEMA_VERSION);
}

#[test]
fn older_metadata_and_preview_manifests_receive_safe_defaults() {
    let metadata: RemotePhotoMetadata = serde_json::from_str(
        r#"{"camera_make":"Legacy","camera_model":"Camera","lens_make":"","lens_model":"","captured_at_unix_seconds":null,"iso_speed":null,"exposure_time_seconds":null,"aperture_f_number":null,"focal_length_mm":null,"raw_dimensions":null}"#,
    )
    .expect("decode older metadata");
    assert_eq!(
        metadata.schema_version,
        REMOTE_PHOTO_METADATA_SCHEMA_VERSION
    );
    assert!(metadata.orientation.is_none());
    assert!(metadata.gps.is_none());

    let preview: RemotePreviewManifest = serde_json::from_str(
        r#"{"role":"generated_proxy","digest_blake3":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"byte_len":1,"codec":"jpeg","dimensions":{"width":1,"height":1}}"#,
    )
    .expect("decode older preview manifest");
    assert_eq!(
        preview.pixel_orientation,
        RemotePreviewPixelOrientation::EncodedMetadata
    );
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

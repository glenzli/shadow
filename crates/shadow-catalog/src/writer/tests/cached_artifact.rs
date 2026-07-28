use shadow_domain::{
    AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RepresentationKind,
};

use crate::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, InvalidateCachedArtifactStatus,
    LiveCachedArtifactBlob, RecordCachedArtifact, RecordCachedArtifactStatus, RegisterAsset,
    RepresentationFingerprint,
};

use crate::writer::CatalogActor;

#[test]
#[allow(clippy::too_many_lines)]
fn actor_routes_the_complete_cached_artifact_reference_lifecycle() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let source = RepresentationFingerprint {
        byte_len: 1_024,
        modified_at_ms: Some(123),
    };
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/cache-artifact.dng".to_vec(),
                "/photos/cache-artifact.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register source through actor");
    let artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: "grid-jpeg-2048".into(),
        generator_id: "shadow-proxy".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: "blake3-256".into(),
        blob_digest: [17; 32],
        blob_byte_len: 2_048,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 2_048,
            height: 1_365,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 200,
    };
    let request = RecordCachedArtifact {
        representation_id: registered.representation_id,
        expected_source: source,
        artifact: artifact.clone(),
    };

    assert_eq!(
        handle
            .record_cached_artifact(&request)
            .expect("record artifact through actor"),
        RecordCachedArtifactStatus::Recorded
    );
    let record = CachedArtifactRecord {
        representation_id: registered.representation_id,
        source,
        artifact,
    };
    assert_eq!(
        handle
            .cached_artifacts(registered.representation_id)
            .expect("list artifacts through actor"),
        vec![record.clone()]
    );
    assert_eq!(
        handle
            .preferred_cached_artifact(registered.representation_id)
            .expect("select preferred artifact through actor"),
        Some(record.clone())
    );
    assert_eq!(
        handle
            .preferred_cached_artifacts(&[
                registered.representation_id,
                registered.representation_id,
            ])
            .expect("select preferred artifact batch through actor"),
        vec![Some(record.clone()), Some(record.clone())]
    );
    assert_eq!(
        handle
            .live_cached_artifact_blobs()
            .expect("list live blobs through actor"),
        vec![LiveCachedArtifactBlob {
            algorithm: "blake3-256".into(),
            digest: [17; 32],
        }]
    );
    assert_eq!(
        handle
            .invalidate_cached_artifact(&record)
            .expect("invalidate artifact through actor"),
        InvalidateCachedArtifactStatus::Invalidated
    );
    assert_eq!(
        handle
            .invalidate_cached_artifact(&record)
            .expect("repeat conditional invalidation through actor"),
        InvalidateCachedArtifactStatus::NotCurrent
    );
    assert!(
        handle
            .cached_artifacts(registered.representation_id)
            .expect("read empty artifacts through actor")
            .is_empty()
    );
    actor.shutdown().expect("shutdown actor");
}

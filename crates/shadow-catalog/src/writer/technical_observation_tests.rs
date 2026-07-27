use shadow_ai::{DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, observe_display_luma};
use shadow_domain::{
    AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RepresentationKind,
};

use crate::{
    CachedArtifact, CachedArtifactRole, RecordCachedArtifact, RecordTechnicalObservation,
    RecordTechnicalObservationStatus, RegisterAsset, RepresentationFingerprint,
    TechnicalObservationRevision, technical_observation::artifact_content_hash,
};

use super::CatalogActor;

#[test]
fn actor_round_trips_one_exact_revisioned_technical_observation() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let source = RepresentationFingerprint {
        byte_len: 4_096,
        modified_at_ms: Some(123),
    };
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/technical-observation.dng".to_vec(),
                "/photos/technical-observation.dng",
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
        blob_digest: [29; 32],
        blob_byte_len: 2_048,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 2_048,
            height: 1_365,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 150,
    };
    handle
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: registered.representation_id,
            expected_source: source,
            artifact: artifact.clone(),
        })
        .expect("record source visual through actor");
    let preprocessing_version = "jpeg-luma-v1";
    let input_source_hash = artifact_content_hash(&artifact);
    let samples = [0.0, 0.25, 0.75, 1.0];
    let observation = observe_display_luma(DisplayLumaPlane {
        contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
        width: 2,
        height: 2,
        stride: 2,
        samples: &samples,
        preprocessing_version,
        input_source_hash: &input_source_hash,
    })
    .expect("observe display luma");
    let request = RecordTechnicalObservation {
        representation_id: registered.representation_id,
        expected_source: source,
        expected_artifact: artifact.clone(),
        observation: observation.clone(),
        observed_at_ms: 200,
    };

    assert_eq!(
        handle
            .record_technical_observation(&request)
            .expect("record technical observation through actor"),
        RecordTechnicalObservationStatus::Recorded
    );
    let record = handle
        .technical_observation(
            registered.representation_id,
            source,
            &artifact,
            &TechnicalObservationRevision::current(preprocessing_version),
        )
        .expect("read technical observation through actor")
        .expect("exact observation remains current");
    assert_eq!(record.representation_id, registered.representation_id);
    assert_eq!(record.source, source);
    assert_eq!(record.source_artifact, artifact);
    assert_eq!(record.observation, observation);
    assert_eq!(record.observed_at_ms, 200);
    actor.shutdown().expect("shutdown actor");
}

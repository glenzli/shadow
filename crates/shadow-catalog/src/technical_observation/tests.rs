use shadow_ai::{
    DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, NonNegativeFinite, observe_display_luma,
};
use shadow_domain::{
    AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RepresentationKind,
};

use super::*;
use crate::{CachedArtifactRole, RecordCachedArtifact, RegisterAsset};

#[test]
fn exact_observation_round_trips_and_idempotently_updates_one_identity() {
    let (mut catalog, representation_id, source, artifact) = fixture();
    let first = request(representation_id, source, &artifact, "jpeg-luma-v1", 200);
    assert_eq!(
        catalog
            .record_technical_observation(&first)
            .expect("record first observation"),
        RecordTechnicalObservationStatus::Recorded
    );
    let mut repeated = first.clone();
    repeated.observed_at_ms = 201;
    assert_eq!(
        catalog
            .record_technical_observation(&repeated)
            .expect("repeat observation"),
        RecordTechnicalObservationStatus::Recorded
    );
    let revision = TechnicalObservationRevision::current("jpeg-luma-v1");
    let record = catalog
        .technical_observation(representation_id, source, &artifact, &revision)
        .expect("read observation")
        .expect("current observation");
    assert_eq!(record.observed_at_ms, 201);
    assert_eq!(record.observation, repeated.observation);
    let count: i64 = catalog
        .connection
        .query_row(
            "SELECT COUNT(*) FROM representation_technical_observations",
            [],
            |row| row.get(0),
        )
        .expect("count observations");
    assert_eq!(count, 1);
}

#[test]
fn persisted_float_integrity_does_not_require_textual_reserialization_stability() {
    let (mut catalog, representation_id, source, artifact) = fixture();
    let mut request = request(representation_id, source, &artifact, "jpeg-luma-v1", 200);
    request.observation.metrics.edge_energy =
        NonNegativeFinite::new(0.000_371_511_986_703_282_44).expect("valid real metric");
    catalog
        .record_technical_observation(&request)
        .expect("record real floating-point observation");

    let record = catalog
        .technical_observation(
            representation_id,
            source,
            &artifact,
            &TechnicalObservationRevision::current("jpeg-luma-v1"),
        )
        .expect("read digest-valid typed JSON")
        .expect("observation exists");
    assert!(
        (record.observation.metrics.edge_energy.get()
            - request.observation.metrics.edge_energy.get())
        .abs()
            < 1.0e-18
    );
}

#[test]
fn stale_source_or_replaced_exact_artifact_is_not_recorded() {
    let (mut catalog, representation_id, source, artifact) = fixture();
    let stale_source = RecordTechnicalObservation {
        expected_source: RepresentationFingerprint {
            byte_len: source.byte_len + 1,
            modified_at_ms: source.modified_at_ms,
        },
        ..request(representation_id, source, &artifact, "jpeg-luma-v1", 200)
    };
    assert_eq!(
        catalog
            .record_technical_observation(&stale_source)
            .expect("reject stale source"),
        RecordTechnicalObservationStatus::StaleInput
    );

    let mut replacement = artifact.clone();
    replacement.generator_version = "2".into();
    replacement.created_at_ms += 1;
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: replacement,
        })
        .expect("replace visual");
    assert_eq!(
        catalog
            .record_technical_observation(&request(
                representation_id,
                source,
                &artifact,
                "jpeg-luma-v1",
                201,
            ))
            .expect("reject replaced artifact"),
        RecordTechnicalObservationStatus::StaleInput
    );
}

#[test]
fn revisions_coexist_and_corrupt_current_payload_fails_integrity_check() {
    let (mut catalog, representation_id, source, artifact) = fixture();
    for (preprocessing, observed_at_ms) in [("jpeg-luma-v1", 200), ("jpeg-luma-v2", 201)] {
        catalog
            .record_technical_observation(&request(
                representation_id,
                source,
                &artifact,
                preprocessing,
                observed_at_ms,
            ))
            .expect("record revision");
    }
    for preprocessing in ["jpeg-luma-v1", "jpeg-luma-v2"] {
        assert!(
            catalog
                .technical_observation(
                    representation_id,
                    source,
                    &artifact,
                    &TechnicalObservationRevision::current(preprocessing),
                )
                .expect("read exact revision")
                .is_some()
        );
    }
    let unsupported = TechnicalObservationRevision {
        observation_schema: TECHNICAL_QUALITY_SCHEMA_VERSION + 1,
        ..TechnicalObservationRevision::current("jpeg-luma-v2")
    };
    assert!(
        catalog
            .technical_observation(representation_id, source, &artifact, &unsupported)
            .expect("unknown revisions are absent")
            .is_none()
    );

    catalog
        .connection
        .execute(
            "UPDATE representation_technical_observations
             SET observation_digest = zeroblob(32)
             WHERE preprocessing_version = 'jpeg-luma-v2'",
            [],
        )
        .expect("corrupt digest fixture");
    assert!(matches!(
        catalog.technical_observation(
            representation_id,
            source,
            &artifact,
            &TechnicalObservationRevision::current("jpeg-luma-v2"),
        ),
        Err(CatalogError::InvalidPersistedTechnicalObservation(
            "observation JSON digest does not match"
        ))
    ));
}

fn fixture() -> (
    Catalog,
    RepresentationId,
    RepresentationFingerprint,
    CachedArtifact,
) {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let source = RepresentationFingerprint {
        byte_len: 4_096,
        modified_at_ms: Some(123),
    };
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/technical.dng".to_vec(),
                "/photos/technical.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register source");
    let artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: "libraw:grid-jpeg-2048-q88-444-v3".into(),
        generator_id: "libraw".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: "blake3-256".into(),
        blob_digest: [7; 32],
        blob_byte_len: 1_024,
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
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: registered.representation_id,
            expected_source: source,
            artifact: artifact.clone(),
        })
        .expect("record visual");
    (catalog, registered.representation_id, source, artifact)
}

fn request(
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    artifact: &CachedArtifact,
    preprocessing_version: &str,
    observed_at_ms: i64,
) -> RecordTechnicalObservation {
    let input_source_hash = artifact_content_hash(artifact);
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
    .expect("observe luma");
    RecordTechnicalObservation {
        representation_id,
        expected_source: source,
        expected_artifact: artifact.clone(),
        observation,
        observed_at_ms,
    }
}

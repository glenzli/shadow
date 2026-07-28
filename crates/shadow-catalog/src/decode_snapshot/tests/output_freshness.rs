use shadow_ai::{DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, observe_display_luma};
use shadow_domain::{DecodeSupport, ImageDimensions, PreviewCodec, RepresentationId};

use crate::{
    CachedArtifact, CachedArtifactRole, Catalog, RecordCachedArtifact, RecordTechnicalObservation,
    technical_observation::artifact_content_hash,
};

use super::{
    super::{RecordDecodeSnapshot, RepresentationFingerprint},
    fixture::{registered_catalog, snapshot},
};

#[test]
#[allow(clippy::too_many_lines)]
fn embedded_placeholder_does_not_suppress_generated_proxy_backfill() {
    const PROXY_KEY: &str = "libraw:grid-jpeg-2048-q88-444-v3";
    let (mut catalog, representation_id, source) = registered_catalog();
    catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: source,
            snapshot: snapshot("libraw", "1", &[7]),
            inspected_at_ms: 456,
        })
        .expect("record snapshot");

    assert!(
        catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                false,
                PROXY_KEY,
                None,
            )
            .expect("query descriptor-only state")
    );
    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "2",
                source,
                false,
                PROXY_KEY,
                None,
            )
            .expect("query newer provider version")
    );
    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                None,
            )
            .expect("query missing cached preview")
    );

    let embedded_artifact = CachedArtifact {
        role: CachedArtifactRole::EmbeddedPreview,
        variant_key: "libraw".into(),
        generator_id: "libraw".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: Some(7),
        blob_algorithm: "blake3-256".into(),
        blob_digest: [1; 32],
        blob_byte_len: 1_024,
        codec: PreviewCodec::Jpeg,
        byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 1_600,
            height: 1_200,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 789,
    };
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: embedded_artifact,
        })
        .expect("record cached preview");

    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                None,
            )
            .expect("embedded placeholder still requires a developed proxy")
    );
    let generated_artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: PROXY_KEY.into(),
        generator_id: "libraw".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: "blake3-256".into(),
        blob_digest: [2; 32],
        blob_byte_len: 456_789,
        codec: PreviewCodec::Jpeg,
        byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 2_048,
            height: 1_365,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 790,
    };
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: generated_artifact.clone(),
        })
        .expect("record developed proxy");
    assert!(
        catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                None,
            )
            .expect("current developed proxy completes cached output")
    );
    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                Some("jpeg-luma-v1"),
            )
            .expect("migration backfill requires technical observation")
    );
    record_technical(
        &mut catalog,
        representation_id,
        source,
        &generated_artifact,
        "jpeg-luma-v1",
    );
    assert!(
        catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                Some("jpeg-luma-v1"),
            )
            .expect("matching technical observation completes backfill")
    );
    catalog
        .connection
        .execute(
            "UPDATE representation_technical_observations
             SET observation_digest = zeroblob(32)
             WHERE preprocessing_version = 'jpeg-luma-v1'",
            [],
        )
        .expect("corrupt rebuildable technical observation");
    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                Some("jpeg-luma-v1"),
            )
            .expect("corrupt observation requires regeneration")
    );
    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                Some("jpeg-luma-v2"),
            )
            .expect("preprocessing upgrade requires backfill")
    );
}

#[test]
#[allow(clippy::too_many_lines)]
fn generated_proxy_requirement_uses_the_current_recipe_and_provider_version() {
    const PROXY_KEY: &str = "libraw:grid-jpeg-2048-q88-444-v3";
    let (mut catalog, representation_id, source) = registered_catalog();
    let mut without_preview = snapshot("libraw", "1", &[]);
    without_preview.capabilities.embedded_previews = DecodeSupport::Unavailable;
    catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: source,
            snapshot: without_preview,
            inspected_at_ms: 456,
        })
        .expect("record snapshot");

    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                None,
            )
            .expect("query missing generated proxy")
    );

    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: CachedArtifact {
                role: CachedArtifactRole::GeneratedProxy,
                variant_key: PROXY_KEY.into(),
                generator_id: "libraw".into(),
                generator_version: "stale".into(),
                recipe_snapshot_digest: None,
                provider_preview_id: None,
                blob_algorithm: "blake3-256".into(),
                blob_digest: [1; 32],
                blob_byte_len: 456_789,
                codec: PreviewCodec::Jpeg,
                byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
                dimensions: ImageDimensions {
                    width: 2_048,
                    height: 1_365,
                },
                bits_per_channel: 8,
                channels: 3,
                created_at_ms: 788,
            },
        })
        .expect("record generated proxy from stale provider version");

    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                None,
            )
            .expect("generator version mismatch must not hit")
    );

    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: CachedArtifact {
                role: CachedArtifactRole::GeneratedProxy,
                variant_key: PROXY_KEY.into(),
                generator_id: "libraw".into(),
                generator_version: "1".into(),
                recipe_snapshot_digest: None,
                provider_preview_id: None,
                blob_algorithm: "blake3-256".into(),
                blob_digest: [2; 32],
                blob_byte_len: 456_789,
                codec: PreviewCodec::Jpeg,
                byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
                dimensions: ImageDimensions {
                    width: 2_048,
                    height: 1_365,
                },
                bits_per_channel: 8,
                channels: 3,
                created_at_ms: 789,
            },
        })
        .expect("record generated proxy");

    assert!(
        catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                None,
            )
            .expect("query complete generated proxy")
    );
    assert!(
        !catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                "libraw:grid-jpeg-2048-q88-444-v4",
                None,
            )
            .expect("query newer proxy recipe")
    );
}

#[test]
fn non_jpeg_visual_does_not_create_a_technical_backfill_loop() {
    const PROXY_KEY: &str = "libraw:grid-bitmap-2048-v1";
    let (mut catalog, representation_id, source) = registered_catalog();
    let mut without_preview = snapshot("libraw", "1", &[]);
    without_preview.capabilities.embedded_previews = DecodeSupport::Unavailable;
    catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: source,
            snapshot: without_preview,
            inspected_at_ms: 456,
        })
        .expect("record snapshot");
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: CachedArtifact {
                role: CachedArtifactRole::GeneratedProxy,
                variant_key: PROXY_KEY.into(),
                generator_id: "libraw".into(),
                generator_version: "1".into(),
                recipe_snapshot_digest: None,
                provider_preview_id: None,
                blob_algorithm: "blake3-256".into(),
                blob_digest: [3; 32],
                blob_byte_len: 456_789,
                codec: PreviewCodec::Bitmap,
                byte_order: shadow_domain::PreviewByteOrder::Native,
                dimensions: ImageDimensions {
                    width: 2_048,
                    height: 1_365,
                },
                bits_per_channel: 8,
                channels: 3,
                created_at_ms: 790,
            },
        })
        .expect("record non-JPEG visual");
    assert!(
        catalog
            .is_decode_output_current(
                representation_id,
                "libraw",
                "1",
                source,
                true,
                PROXY_KEY,
                Some("jpeg-luma-v1"),
            )
            .expect("non-JPEG visuals do not create an impossible backfill loop")
    );
}

fn record_technical(
    catalog: &mut Catalog,
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    artifact: &CachedArtifact,
    preprocessing_version: &str,
) {
    let input_source_hash = artifact_content_hash(artifact);
    let observation = observe_display_luma(DisplayLumaPlane {
        contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
        width: 2,
        height: 2,
        stride: 2,
        samples: &[0.0, 0.25, 0.75, 1.0],
        preprocessing_version,
        input_source_hash: &input_source_hash,
    })
    .expect("observe luma");
    catalog
        .record_technical_observation(&RecordTechnicalObservation {
            representation_id,
            expected_source: source,
            expected_artifact: artifact.clone(),
            observation,
            observed_at_ms: 900,
        })
        .expect("record technical observation");
}

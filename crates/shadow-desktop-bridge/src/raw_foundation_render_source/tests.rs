use std::path::PathBuf;

use shadow_ai::{
    ArtifactHashAlgorithm, GeneratedArtifactReference, RAW_FOUNDATION_ENCODING_VERSION,
    RAW_FOUNDATION_MEDIA_TYPE, RasterExtent, RawFoundationArtifact, RawFoundationProvenance,
    RawFoundationSourceProvenance,
};
use shadow_cache::FoundationArtifactVerification;
use shadow_domain::RawFoundationDenoiseModel;

use super::*;

fn descriptor() -> RawFoundationArtifact {
    RawFoundationArtifact::new(
        GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Sha256,
            "1".repeat(64),
            4_096,
            RAW_FOUNDATION_MEDIA_TYPE.into(),
            RAW_FOUNDATION_ENCODING_VERSION,
        )
        .expect("artifact reference"),
        RasterExtent::new(6, 4).expect("extent"),
        RawFoundationSourceProvenance::new("2".repeat(64), 8_192, "3".repeat(64))
            .expect("source provenance"),
        RawFoundationProvenance::new(
            "4".repeat(64),
            "5".repeat(64),
            RawFoundationDenoiseModel::RAWNIND_PACKAGE_SHA256.into(),
            RawFoundationDenoiseModel::RAWNIND_BAYER_GRAPH_SHA256.into(),
            RAW_FOUNDATION_IMPLEMENTATION_REVISION.into(),
        )
        .expect("foundation provenance"),
    )
    .expect("foundation descriptor")
}

fn verification() -> FoundationArtifactVerification {
    FoundationArtifactVerification {
        path: PathBuf::from("/cache/foundation.shadowrawf"),
        width: 6,
        height: 4,
        force_rggb_crop_sensor: [0, 1],
        source_sha256: "2".repeat(64),
        source_size_bytes: 8_192,
        source_pixel_contract_sha256: "3".repeat(64),
        model_package_sha256: RawFoundationDenoiseModel::RAWNIND_PACKAGE_SHA256.into(),
        model_graph_sha256: RawFoundationDenoiseModel::RAWNIND_BAYER_GRAPH_SHA256.into(),
        implementation_revision: RAW_FOUNDATION_IMPLEMENTATION_REVISION.into(),
        cache_key_sha256: "4".repeat(64),
        artifact_identity_sha256: "5".repeat(64),
        sequence_sha256: "8".repeat(64),
        payload_bytes: 288,
        file_bytes: 4_096,
        file_sha256: "1".repeat(64),
        stripes: Vec::new(),
    }
}

#[test]
fn complete_descriptor_and_verified_artifact_contract_match() {
    ensure_descriptor_matches(&verification(), &descriptor()).expect("matching descriptor");
}

#[test]
fn substituted_model_or_artifact_fails_closed() {
    let mut substituted_model = verification();
    substituted_model.model_graph_sha256 = "9".repeat(64);
    assert!(
        ensure_descriptor_matches(&substituted_model, &descriptor())
            .expect_err("substituted model must fail")
            .to_string()
            .contains("model or cache provenance")
    );

    let mut substituted_artifact = verification();
    substituted_artifact.file_sha256 = "a".repeat(64);
    assert!(
        ensure_descriptor_matches(&substituted_artifact, &descriptor())
            .expect_err("substituted artifact must fail")
            .to_string()
            .contains("artifact bytes")
    );
}

#[test]
fn cache_identity_covers_source_artifact_model_and_implementation() {
    let ready = RawFoundationReady {
        descriptor: descriptor(),
        path: PathBuf::from("/cache/foundation.shadowrawf"),
        source_path: PathBuf::from("/source/input.nef"),
        source: RepresentationFingerprint {
            byte_len: 8_192,
            modified_at_ms: Some(17),
        },
        disposition: shadow_ai::RawFoundationMaterializationDisposition::Published,
    };
    let identity = RawFoundationRenderIdentity::from_ready(
        &ready,
        RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
    )
    .expect("render identity");

    assert_eq!(identity.source_sha256, "2".repeat(64));
    assert_eq!(identity.artifact_file_sha256, "1".repeat(64));
    assert_eq!(identity.cache_key_sha256, "4".repeat(64));
    assert_eq!(identity.artifact_identity_sha256, "5".repeat(64));
    assert_eq!(
        identity.model_package_sha256,
        RawFoundationDenoiseModel::RAWNIND_PACKAGE_SHA256
    );
    assert_eq!(
        identity.model_graph_sha256,
        RawFoundationDenoiseModel::RAWNIND_BAYER_GRAPH_SHA256
    );
    assert_eq!(
        identity.implementation_revision,
        RAW_FOUNDATION_IMPLEMENTATION_REVISION
    );
}

#[test]
fn disabled_recipe_bypasses_while_enabled_recipe_without_ready_output_fails_closed() {
    assert!(
        select_ready_raw_foundation(None, RawFoundationDenoise::disabled())
            .expect("disabled bypass")
            .is_none()
    );
    let error = select_ready_raw_foundation(
        None,
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0),
    )
    .expect_err("enabled intent requires a ready artifact");
    assert!(
        error
            .to_string()
            .contains("no verified foundation is cached")
    );
}

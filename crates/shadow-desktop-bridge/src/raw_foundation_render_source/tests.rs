use std::path::PathBuf;

use shadow_ai::{
    ArtifactHashAlgorithm, GeneratedArtifactReference, RAW_FOUNDATION_ENCODING_VERSION,
    RAW_FOUNDATION_MEDIA_TYPE, RasterExtent, RawFoundationArtifact, RawFoundationProvenance,
    RawFoundationSourceProvenance,
};
use shadow_bridge::{
    BasicEditParameters, CancellableEditPreview, EditPreviewCancellation, OpticsSettings,
    PhotoEditPreviewSession, RawDevelopmentPlan, RawFoundationArtifactIdentity,
    VerifiedRawFoundation, basic_adjustment_render_plan,
};
use shadow_cache::{FoundationArtifactReader, FoundationArtifactVerification};
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
        verified_reader: None,
        raw_frame_staging: None,
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
    let repeated = RawFoundationRenderIdentity::from_ready(
        &ready,
        RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
    )
    .expect("repeated render identity");
    assert_eq!(
        repeated, identity,
        "artifact/source identity is independent from the authored live mix amount"
    );
}

#[test]
fn disabled_recipe_bypasses_while_enabled_recipe_without_ready_output_fails_closed() {
    assert!(
        select_ready_raw_foundation(None, RawFoundationDenoise::disabled())
            .expect("disabled bypass")
            .is_none()
    );
    let zero_amount =
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0)
            .with_amount_percent(0)
            .expect("zero amount");
    assert!(
        select_ready_raw_foundation(None, zero_amount)
            .expect("zero amount is an exact bypass")
            .is_none()
    );
    let hidden =
        RawFoundationDenoise::enabled(RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0)
            .with_bypassed(true);
    assert!(hidden.is_enabled());
    assert!(
        select_ready_raw_foundation(None, hidden)
            .expect("hidden node is an exact bypass")
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

#[test]
#[allow(clippy::cast_precision_loss, clippy::too_many_lines)]
fn real_cached_foundation_materially_changes_final_preview_pixels() {
    let (Some(source_path), Some(artifact_path)) = (
        std::env::var_os("SHADOW_TEST_RAW_FOUNDATION_SOURCE"),
        std::env::var_os("SHADOW_TEST_FOUNDATION_ARTIFACT"),
    ) else {
        return;
    };
    let source_path = PathBuf::from(source_path);
    let staging_manifest =
        std::env::var_os("SHADOW_TEST_RAW_FOUNDATION_STAGING").map(PathBuf::from);
    let mut reader = FoundationArtifactReader::open(PathBuf::from(artifact_path))
        .expect("verify real cached foundation");
    let verification = reader.verification().clone();
    let mut foundation = |amount_percent| {
        let samples = reader
            .read_interleaved_rows(0, verification.height)
            .expect("read complete real cached foundation");
        let identity = RawFoundationArtifactIdentity::from_verified_digests(
            verification.source_sha256.clone(),
            verification.file_sha256.clone(),
            verification.cache_key_sha256.clone(),
        )
        .expect("bind real cached foundation identity");
        VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
            ImageDimensions {
                width: verification.width,
                height: verification.height,
            },
            verification.force_rggb_crop_sensor[0],
            verification.force_rggb_crop_sensor[1],
            amount_percent,
            identity,
            samples,
        )
        .expect("build real cached foundation transfer")
    };
    let development = RawDevelopmentPlan::preview();
    let optics = OpticsSettings::default();
    let ordinary = if let Some(staging_manifest) = staging_manifest.as_deref() {
        PhotoEditPreviewSession::open_with_staged_raw_foundation(
            &source_path,
            staging_manifest,
            1_024,
            development,
            &foundation(0),
            &optics,
        )
        .expect("prepare staged original-RAW preview")
    } else {
        PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
            &source_path,
            1_024,
            development,
            &optics,
        )
        .expect("prepare ordinary RAW preview")
    };
    let denoised_foundation = foundation(100);
    let denoised = match staging_manifest.as_deref() {
        Some(staging_manifest) => PhotoEditPreviewSession::open_with_staged_raw_foundation(
            &source_path,
            staging_manifest,
            1_024,
            development,
            &denoised_foundation,
            &optics,
        ),
        None => PhotoEditPreviewSession::open_with_raw_foundation(
            &source_path,
            1_024,
            development,
            &denoised_foundation,
            &optics,
        ),
    }
    .expect("prepare AI-foundation RAW preview");
    let plan = basic_adjustment_render_plan(BasicEditParameters {
        exposure_stops: 3.75,
        contrast_factor: 0.59,
        ..BasicEditParameters::default()
    })
    .expect("compile representative shadow-lift preview plan");
    let ordinary = ordinary
        .render_plan_rgb8_cancellable(
            &plan,
            &EditPreviewCancellation::new().expect("ordinary cancellation handle"),
        )
        .expect("render ordinary RAW preview");
    let denoised = denoised
        .render_plan_rgb8_cancellable(
            &plan,
            &EditPreviewCancellation::new().expect("AI cancellation handle"),
        )
        .expect("render AI-foundation preview");
    let CancellableEditPreview::Completed(ordinary) = ordinary else {
        panic!("ordinary preview was unexpectedly cancelled");
    };
    let CancellableEditPreview::Completed(denoised) = denoised else {
        panic!("AI-foundation preview was unexpectedly cancelled");
    };
    assert_eq!(ordinary.dimensions, denoised.dimensions);
    assert_eq!(ordinary.bytes.len(), denoised.bytes.len());
    let changed = ordinary
        .bytes
        .iter()
        .zip(&denoised.bytes)
        .filter(|(before, after)| before != after)
        .count();
    let absolute_difference: u64 = ordinary
        .bytes
        .iter()
        .zip(&denoised.bytes)
        .map(|(before, after)| u64::from(before.abs_diff(*after)))
        .sum();
    let mean_absolute_difference = absolute_difference as f64 / ordinary.bytes.len() as f64;
    let patch_chroma_variation = |bytes: &[u8]| {
        let width = ordinary.dimensions.width as usize;
        let height = ordinary.dimensions.height as usize;
        let left = width / 20;
        let right = width * 3 / 10;
        let top = height / 20;
        let bottom = height * 3 / 10;
        let chroma = |x: usize, y: usize, channel: usize| {
            let pixel = (y * width + x) * 3;
            i16::from(bytes[pixel + channel]) - i16::from(bytes[pixel + 1])
        };
        let mut total = 0_u64;
        let mut samples = 0_u64;
        for y in top + 1..bottom {
            for x in left + 1..right {
                for channel in [0_usize, 2_usize] {
                    let current = chroma(x, y, channel);
                    total += u64::from(current.abs_diff(chroma(x - 1, y, channel)));
                    total += u64::from(current.abs_diff(chroma(x, y - 1, channel)));
                    samples += 2;
                }
            }
        }
        total as f64 / samples as f64
    };
    let ordinary_chroma_variation = patch_chroma_variation(&ordinary.bytes);
    let denoised_chroma_variation = patch_chroma_variation(&denoised.bytes);
    eprintln!(
        "real AI RAW preview difference: changed={changed}/{} mean-absolute-difference={mean_absolute_difference:.4} sky-chroma-variation={ordinary_chroma_variation:.4}->{denoised_chroma_variation:.4}",
        ordinary.bytes.len()
    );
    assert!(
        changed > ordinary.bytes.len() / 100,
        "the verified AI RAW foundation changed at most one percent of final preview samples"
    );
    assert!(
        mean_absolute_difference >= 0.5,
        "the verified AI RAW foundation changed final preview samples only trivially"
    );
    assert!(
        denoised_chroma_variation < ordinary_chroma_variation * 0.95,
        "the verified AI RAW foundation does not materially reduce representative sky chroma variation"
    );
}

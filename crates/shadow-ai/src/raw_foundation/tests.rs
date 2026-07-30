use shadow_domain::ImageDomain;

use super::*;
use crate::{DenoisedRasterArtifact, TileContract};

fn generated_reference(
    hash_algorithm: ArtifactHashAlgorithm,
    content_hash: String,
    media_type: &str,
    encoding_version: u32,
) -> GeneratedArtifactReference {
    GeneratedArtifactReference::new(
        hash_algorithm,
        content_hash,
        216_000_000,
        media_type.to_owned(),
        encoding_version,
    )
    .expect("syntactically valid generated reference")
}

fn source() -> RawFoundationSourceProvenance {
    RawFoundationSourceProvenance::new("a".repeat(64), 42_000_000, "b".repeat(64))
        .expect("valid source provenance")
}

fn provenance() -> RawFoundationProvenance {
    RawFoundationProvenance::new(
        "c".repeat(64),
        "d".repeat(64),
        "e".repeat(64),
        "f".repeat(64),
        "rawnind-public-bayer-foundation-20260731.1".into(),
    )
    .expect("valid foundation provenance")
}

fn foundation() -> RawFoundationArtifact {
    RawFoundationArtifact::new(
        generated_reference(
            ArtifactHashAlgorithm::Sha256,
            "1".repeat(64),
            RAW_FOUNDATION_MEDIA_TYPE,
            RAW_FOUNDATION_ENCODING_VERSION,
        ),
        RasterExtent::new(6000, 4000).expect("valid extent"),
        source(),
        provenance(),
    )
    .expect("valid RAW foundation")
}

#[test]
fn foundation_is_a_fixed_full_resolution_domain_transition() {
    let foundation = foundation();

    assert_eq!(foundation.input_domain(), ImageDomain::SensorMosaic);
    assert_eq!(foundation.output_domain(), ImageDomain::SceneLinearRgb);
    assert_eq!(foundation.pixel_layout(), RasterPixelLayout::RgbPlanar);
    assert_eq!(foundation.sample_format(), RasterSampleFormat::Float32);
    assert!(foundation.is_full_resolution());
    assert_eq!(foundation.raster_extent().width, 6000);
    assert_eq!(foundation.source().source_size_bytes(), 42_000_000);
    assert_eq!(
        foundation.provenance().implementation_revision(),
        "rawnind-public-bayer-foundation-20260731.1"
    );
}

#[test]
fn serialized_foundation_preserves_provenance_but_never_a_local_path() {
    let encoded = serde_json::to_string(&foundation()).expect("serialize foundation");
    let decoded: RawFoundationArtifact =
        serde_json::from_str(&encoded).expect("deserialize foundation");

    decoded.validate().expect("validate restored foundation");
    assert_eq!(decoded, foundation());
    assert!(!encoded.contains("/private/"));
    assert!(!encoded.contains("cache_path"));
}

#[test]
fn foundation_rejects_substituted_encoding_and_noncanonical_provenance() {
    let wrong_media = RawFoundationArtifact::new(
        generated_reference(
            ArtifactHashAlgorithm::Sha256,
            "1".repeat(64),
            "image/tiff",
            RAW_FOUNDATION_ENCODING_VERSION,
        ),
        RasterExtent::new(6000, 4000).expect("valid extent"),
        source(),
        provenance(),
    );
    assert_eq!(
        wrong_media,
        Err(RawFoundationArtifactError::UnexpectedMediaType)
    );

    let uppercase_digest =
        RawFoundationSourceProvenance::new("A".repeat(64), 42_000_000, "b".repeat(64));
    assert_eq!(
        uppercase_digest,
        Err(RawFoundationArtifactError::InvalidDigest(
            RawFoundationDigestField::SourceFile
        ))
    );
}

#[test]
fn ordinary_denoise_still_cannot_impersonate_a_raw_foundation() {
    let ordinary = DenoisedRasterArtifact {
        artifact: generated_reference(
            ArtifactHashAlgorithm::Sha256,
            "1".repeat(64),
            RAW_FOUNDATION_MEDIA_TYPE,
            RAW_FOUNDATION_ENCODING_VERSION,
        ),
        input_domain: ImageDomain::SensorMosaic,
        output_domain: ImageDomain::SceneLinearRgb,
        raster_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
        pixel_layout: RasterPixelLayout::RgbPlanar,
        sample_format: RasterSampleFormat::Float32,
        source_pixel_contract_hash: "b".repeat(64),
        tile_contract: Some(TileContract {
            tile_edge: 512,
            halo: 32,
        }),
        full_resolution: true,
    };

    assert!(matches!(
        ordinary.validate(),
        Err(AiArtifactContractError::DenoiseChangesImageDomain {
            input: ImageDomain::SensorMosaic,
            output: ImageDomain::SceneLinearRgb,
        })
    ));
}

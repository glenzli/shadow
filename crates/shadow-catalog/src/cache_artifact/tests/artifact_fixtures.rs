use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::super::*;
use crate::RegisterAsset;

pub(super) fn registered_catalog() -> (Catalog, RepresentationId, RepresentationFingerprint) {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let source = RepresentationFingerprint {
        byte_len: 1_024,
        modified_at_ms: Some(123),
    };
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/input.dng".to_vec(),
                "/photos/input.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register asset");
    (catalog, registered.representation_id, source)
}

pub(super) fn artifact(version: &str, digest_byte: u8) -> CachedArtifact {
    CachedArtifact {
        role: CachedArtifactRole::EmbeddedPreview,
        variant_key: "libraw".into(),
        generator_id: "libraw".into(),
        generator_version: version.into(),
        recipe_snapshot_digest: None,
        provider_preview_id: Some(7),
        blob_algorithm: "blake3-256".into(),
        blob_digest: [digest_byte; 32],
        blob_byte_len: 1_024,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 1_600,
            height: 1_200,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 456,
    }
}

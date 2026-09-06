use shadow_catalog::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, RepresentationFingerprint,
};
use shadow_domain::{EntityId, ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationId};

use crate::{
    CachedRemotePreview,
    protocol::{RemotePreviewManifest, RemotePreviewPixelOrientation, RemotePreviewRole},
};

use super::{BrowseVisual, select_browse_visual};

#[test]
fn current_local_recipe_preview_wins_over_remote_proxy() {
    let representation_id = RepresentationId::new_v7();
    let local = CachedArtifactRecord {
        representation_id,
        source: RepresentationFingerprint {
            byte_len: 10,
            modified_at_ms: Some(4),
        },
        artifact: CachedArtifact {
            role: CachedArtifactRole::RecipePreview,
            variant_key: "recipe-grid".to_owned(),
            generator_id: "shadow".to_owned(),
            generator_version: "v1".to_owned(),
            recipe_snapshot_digest: Some([3; 32]),
            provider_preview_id: None,
            blob_algorithm: "blake3-256".to_owned(),
            blob_digest: [4; 32],
            blob_byte_len: 100,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 800,
                height: 600,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 5,
        },
    };
    let remote = CachedRemotePreview {
        manifest: RemotePreviewManifest {
            role: RemotePreviewRole::EmbeddedPreview,
            digest_blake3: [8; 32],
            byte_len: 90,
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 640,
                height: 480,
            },
            pixel_orientation: RemotePreviewPixelOrientation::EncodedMetadata,
        },
    };
    assert!(matches!(
        select_browse_visual(Some(&local), Some(&remote)),
        BrowseVisual::Local(selected) if selected.artifact.role == CachedArtifactRole::RecipePreview
    ));
    assert!(matches!(
        select_browse_visual(None, Some(&remote)),
        BrowseVisual::Remote(_)
    ));
}

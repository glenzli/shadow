use shadow_domain::{
    AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RepresentationKind,
};

use super::artifact_fixtures::{artifact, registered_catalog};
use crate::{
    CachedArtifact, CachedArtifactRole, LiveCachedArtifactBlob, RecordCachedArtifact,
    RegisterAsset, RepresentationFingerprint,
};

#[test]
fn recipe_preview_is_ignored_until_a_working_recipe_names_its_digest() {
    let (mut catalog, representation_id, source) = registered_catalog();
    let source_preview = artifact("1", 1);
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: source_preview,
        })
        .expect("record source preview");

    let recipe_preview = CachedArtifact {
        role: CachedArtifactRole::RecipePreview,
        variant_key: "shadow-recipe-preview:test;recipe=aa".into(),
        generator_id: "shadow-edit-preview".into(),
        generator_version: "test".into(),
        recipe_snapshot_digest: Some([0xaa; 32]),
        provider_preview_id: None,
        blob_algorithm: "blake3-256".into(),
        blob_digest: [2; 32],
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
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: recipe_preview,
        })
        .expect("record unattached Recipe preview");

    assert_eq!(
        catalog
            .preferred_cached_artifact(representation_id)
            .expect("select preferred preview")
            .expect("source preview remains available")
            .artifact
            .role,
        CachedArtifactRole::EmbeddedPreview
    );
}

#[test]
fn preferred_artifact_batch_preserves_request_order_without_per_photo_queries() {
    let (mut catalog, first_representation_id, first_source) = registered_catalog();
    let second_source = RepresentationFingerprint {
        byte_len: 2_048,
        modified_at_ms: Some(456),
    };
    let second = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/second.dng".to_vec(),
                "/photos/second.dng",
            ),
            byte_len: second_source.byte_len,
            modified_at_ms: second_source.modified_at_ms,
            now_ms: 200,
        })
        .expect("register second asset");

    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: first_representation_id,
            expected_source: first_source,
            artifact: artifact("first", 1),
        })
        .expect("record first artifact");
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: second.representation_id,
            expected_source: second_source,
            artifact: artifact("second", 2),
        })
        .expect("record second artifact");

    let selected = catalog
        .preferred_cached_artifacts(&[
            second.representation_id,
            first_representation_id,
            second.representation_id,
        ])
        .expect("select a bounded artifact batch");
    assert_eq!(selected.len(), 3);
    assert_eq!(
        selected[0]
            .as_ref()
            .expect("second artifact")
            .representation_id,
        second.representation_id
    );
    assert_eq!(
        selected[1]
            .as_ref()
            .expect("first artifact")
            .representation_id,
        first_representation_id
    );
    assert_eq!(
        selected[2]
            .as_ref()
            .expect("second artifact duplicate")
            .artifact
            .generator_version,
        "second"
    );
}

#[test]
fn live_blob_listing_excludes_stale_source_artifacts_but_keeps_current_variants() {
    let (mut catalog, representation_id, source) = registered_catalog();
    let current = artifact("current", 1);
    let mut stale = artifact("stale", 2);
    stale.role = CachedArtifactRole::GeneratedProxy;
    stale.variant_key = "generated-proxy".into();
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: current,
        })
        .expect("record current artifact");
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: stale,
        })
        .expect("record proxy artifact");

    catalog
        .connection
        .execute(
            "UPDATE representation_cached_artifacts
             SET source_byte_len = source_byte_len + 1
             WHERE blob_digest = ?1",
            [[2_u8; 32].as_slice()],
        )
        .expect("make one artifact stale without changing its blob");

    let live = catalog
        .live_cached_artifact_blobs()
        .expect("list current cache blobs");
    assert_eq!(
        live,
        vec![LiveCachedArtifactBlob {
            algorithm: "blake3-256".into(),
            digest: [1_u8; 32],
        }]
    );
}

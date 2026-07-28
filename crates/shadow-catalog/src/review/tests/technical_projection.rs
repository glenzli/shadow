use shadow_ai::{DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, observe_display_luma};
use shadow_domain::{
    AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RepresentationKind,
};

use crate::{
    CachedArtifact, CachedArtifactRole, Catalog, RecordCachedArtifact, RecordTechnicalObservation,
    RegisterAsset, RepresentationFingerprint, TechnicalObservationRevision,
    technical_observation::artifact_content_hash,
};

#[test]
fn explicit_technical_revision_keeps_pagination_to_one_row_per_representation() {
    let mut catalog = technical_review_catalog();
    let revision = TechnicalObservationRevision::current("jpeg-luma-v2");
    let first = catalog
        .review_page_with_technical(None, 2, &revision)
        .expect("first Review page");
    assert_eq!(first.total_items, 3);
    assert_eq!(first.items.len(), 2);
    assert!(first.items.iter().all(|item| {
        item.technical
            .as_ref()
            .is_some_and(|summary| summary.preprocessing_version == "jpeg-luma-v2")
    }));
    let second = catalog
        .review_page_with_technical(first.next_cursor.as_ref(), 2, &revision)
        .expect("second Review page");
    assert_eq!(second.items.len(), 1);
    assert!(second.items[0].technical.is_some());
    assert!(second.next_cursor.is_none());

    assert_corrupt_observations_fail_soft(&mut catalog, &revision);
}

fn technical_review_catalog() -> Catalog {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    for (index, path) in ["/photos/c.dng", "/photos/a.dng", "/photos/b.dng"]
        .into_iter()
        .enumerate()
    {
        let source = RepresentationFingerprint {
            byte_len: 4_096,
            modified_at_ms: Some(123),
        };
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register RAW");
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: "proxy-v1".into(),
            generator_id: "libraw".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: "blake3-256".into(),
            blob_digest: [u8::try_from(index + 1).expect("small index"); 32],
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
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: artifact.clone(),
            })
            .expect("record visual");
        for preprocessing_version in ["jpeg-luma-v1", "jpeg-luma-v2"] {
            let input_source_hash = artifact_content_hash(&artifact);
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
                    representation_id: registered.representation_id,
                    expected_source: source,
                    expected_artifact: artifact.clone(),
                    observation,
                    observed_at_ms: 500,
                })
                .expect("record observation");
        }
    }
    catalog
}

fn assert_corrupt_observations_fail_soft(
    catalog: &mut Catalog,
    revision: &TechnicalObservationRevision,
) {
    catalog
        .connection
        .execute(
            "UPDATE representation_technical_observations
             SET observation_digest = zeroblob(32)
             WHERE preprocessing_version = 'jpeg-luma-v2'",
            [],
        )
        .expect("corrupt rebuildable observations");
    let degraded = catalog
        .review_page_with_technical(None, 3, revision)
        .expect("corrupt optional observations do not break Review");
    assert_eq!(degraded.items.len(), 3);
    assert!(degraded.items.iter().all(|item| item.technical.is_none()));

    let without_revision = catalog.review_page(None, 3).expect("plain Review page");
    assert!(
        without_revision
            .items
            .iter()
            .all(|item| item.technical.is_none())
    );
}

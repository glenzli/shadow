use shadow_ai::{DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, observe_display_luma};
use shadow_domain::{
    AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec, RepresentationKind,
};

use crate::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRole, RecordCachedArtifact,
    RecordTechnicalObservation, RegisterAsset, RepresentationFingerprint,
    TechnicalObservationRevision, technical_observation::artifact_content_hash,
};

use crate::writer::CatalogActor;

#[test]
fn actor_routes_plain_and_technical_review_projections() {
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
                b"/photos/review-projection.dng".to_vec(),
                "/photos/review-projection.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register RAW through actor");
    let artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: "review-proxy".into(),
        generator_id: "shadow-proxy".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: "blake3-256".into(),
        blob_digest: [31; 32],
        blob_byte_len: 2_048,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 1_600,
            height: 1_200,
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
        .expect("record Review visual through actor");
    let preprocessing_version = "review-jpeg-luma-v1";
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
    handle
        .record_technical_observation(&RecordTechnicalObservation {
            representation_id: registered.representation_id,
            expected_source: source,
            expected_artifact: artifact.clone(),
            observation,
            observed_at_ms: 200,
        })
        .expect("record technical observation through actor");
    let revision = TechnicalObservationRevision::current(preprocessing_version);

    let plain_page = handle
        .review_page(None, 16)
        .expect("read plain Review page through actor");
    assert_eq!(plain_page.items.len(), 1);
    assert!(plain_page.items[0].technical.is_none());
    assert_eq!(
        plain_page.items[0]
            .visual
            .as_ref()
            .expect("plain page visual")
            .artifact,
        artifact
    );
    let technical_page = handle
        .review_page_with_technical(None, 16, &revision)
        .expect("read technical Review page through actor");
    assert!(technical_page.items[0].technical.is_some());
    let generator_aware_page = handle
        .review_page_with_technical_and_recipe_preview_generator(
            None,
            16,
            &revision,
            &CachedArtifactGeneratorIdentity {
                generator_id: "shadow-edit-preview".into(),
                generator_version: "1".into(),
            },
        )
        .expect("read generator-aware Review page through actor");
    assert!(generator_aware_page.items[0].technical.is_some());
    assert_eq!(
        generator_aware_page.items[0]
            .visual
            .as_ref()
            .expect("fallback generated proxy")
            .artifact,
        artifact
    );

    let plain_source = handle
        .review_source(registered.photo_id)
        .expect("read RAW Review source through actor")
        .expect("online RAW source");
    assert!(plain_source.technical.is_none());
    let technical_source = handle
        .review_source_with_technical(registered.photo_id, &revision)
        .expect("read technical Review source through actor")
        .expect("online technical RAW source");
    assert!(technical_source.technical.is_some());
    let inspection = handle
        .photo_inspection(registered.photo_id, registered.representation_id, &revision)
        .expect("read exact photo inspection through actor")
        .expect("exact online representation");
    assert_eq!(inspection.photo_id, registered.photo_id);
    assert_eq!(inspection.representation_id, registered.representation_id);
    assert!(inspection.technical.is_some());
    assert_eq!(
        handle
            .photo_source(registered.photo_id)
            .expect("read source-neutral photo source through actor")
            .expect("online original source")
            .representation_id,
        registered.representation_id
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_keeps_raster_fallback_out_of_the_legacy_raw_source() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/editable.jpg".to_vec(),
                "/photos/editable.jpg",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register raster through actor");

    assert_eq!(
        handle
            .photo_source(registered.photo_id)
            .expect("resolve source-neutral photo source")
            .expect("online raster source")
            .representation_id,
        registered.representation_id
    );
    assert!(
        handle
            .review_source(registered.photo_id)
            .expect("resolve legacy RAW-only source")
            .is_none()
    );
    actor.shutdown().expect("shutdown actor");
}

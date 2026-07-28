use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact,
    RecordCachedArtifactStatus, RegisterAsset, TechnicalObservationRevision,
};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, RepresentationKind,
};

use super::*;
use crate::display_jpeg_fixture::DISPLAY_JPEG_BYTES;

#[test]
fn worker_persists_observation_for_the_review_preferred_artifact() {
    let ObservationFixture {
        root,
        catalog_actor,
        catalog,
        cache_root,
        source,
        representation_id,
        preferred_artifact,
        embedded_artifact,
    } = observation_fixture();
    let observer = TechnicalObservationActor::spawn_profiled(catalog.clone(), &cache_root)
        .expect("start profiled technical observer");
    let outcome = observer
        .handle()
        .submit_preferred(representation_id, source)
        .expect("submit observation")
        .wait()
        .expect("observe JPEG");
    assert_eq!(outcome, TechnicalObservationOutcome::Recorded);
    let performance = observer
        .shutdown_with_performance()
        .expect("stop profiled technical observer");
    assert!(performance.profiled);
    assert_eq!(performance.technical_observation_total.samples, 1);

    let revision =
        TechnicalObservationRevision::current(technical_analysis_preprocessing_version());
    let persisted = catalog
        .technical_observation(representation_id, source, &preferred_artifact, &revision)
        .expect("read observation")
        .expect("observation exists");
    assert_eq!(persisted.observation.input.width, 2);
    assert_eq!(persisted.observation.input.height, 2);
    assert_eq!(
        persisted.observation.input.preprocessing_version,
        revision.preprocessing_version
    );
    assert!(
        catalog
            .technical_observation(representation_id, source, &embedded_artifact, &revision)
            .expect("read lower-priority observation")
            .is_none()
    );

    finish_fixture(catalog_actor, root);
}

#[test]
fn detached_observation_failure_is_reported_on_shutdown() {
    let ObservationFixture {
        root,
        catalog_actor,
        catalog,
        cache_root,
        source,
        representation_id,
        mut preferred_artifact,
        ..
    } = observation_fixture();
    let observer = TechnicalObservationActor::spawn(catalog, cache_root)
        .expect("start failure-reporting observer");
    preferred_artifact.blob_digest = [9; 32];
    observer
        .handle()
        .sender
        .send(Message::Observe(
            Box::new(CachedArtifactRecord {
                representation_id,
                source,
                artifact: preferred_artifact,
            }),
            Completion::Detached,
        ))
        .expect("submit detached missing cache blob");
    assert!(matches!(
        observer.shutdown(),
        Err(TechnicalObservationError::Cache(_))
    ));

    finish_fixture(catalog_actor, root);
}

struct ObservationFixture {
    root: PathBuf,
    catalog_actor: CatalogActor,
    catalog: CatalogHandle,
    cache_root: PathBuf,
    source: shadow_catalog::RepresentationFingerprint,
    representation_id: RepresentationId,
    preferred_artifact: CachedArtifact,
    embedded_artifact: CachedArtifact,
}

fn observation_fixture() -> ObservationFixture {
    let root = std::env::temp_dir().join(format!(
        "shadow-technical-observer-{}-{}",
        std::process::id(),
        shadow_domain::RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create technical observation fixture");
    let catalog_actor =
        CatalogActor::spawn(&root.join("catalog.sqlite")).expect("start Catalog actor");
    let catalog = catalog_actor.handle();
    let source = shadow_catalog::RepresentationFingerprint {
        byte_len: 4_096,
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
        .expect("register source");
    let cache_root = root.join("cache");
    let cache = ContentAddressedStore::open(&cache_root).expect("open cache");
    let blob = cache.put(DISPLAY_JPEG_BYTES).expect("store JPEG");
    let generated_artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: "test:grid-jpeg-v1".into(),
        generator_id: "test".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: blob.digest.algorithm().into(),
        blob_digest: *blob.digest.as_bytes(),
        blob_byte_len: blob.byte_len,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 2,
            height: 2,
        },
        bits_per_channel: 8,
        channels: 1,
        created_at_ms: 200,
    };
    assert_eq!(
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: generated_artifact.clone(),
            })
            .expect("record cached JPEG"),
        RecordCachedArtifactStatus::Recorded
    );
    let embedded_artifact = CachedArtifact {
        role: CachedArtifactRole::EmbeddedPreview,
        variant_key: "legacy-provider".into(),
        generator_id: "legacy-provider".into(),
        generator_version: "1".into(),
        provider_preview_id: Some(7),
        created_at_ms: 201,
        ..generated_artifact.clone()
    };
    assert_eq!(
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: embedded_artifact.clone(),
            })
            .expect("record old preferred JPEG"),
        RecordCachedArtifactStatus::Recorded
    );
    ObservationFixture {
        root,
        catalog_actor,
        catalog,
        cache_root,
        source,
        representation_id: registered.representation_id,
        preferred_artifact: generated_artifact,
        embedded_artifact,
    }
}

fn finish_fixture(catalog_actor: CatalogActor, root: PathBuf) {
    catalog_actor.shutdown().expect("stop Catalog actor");
    std::fs::remove_dir_all(root).expect("remove technical observation fixture");
}

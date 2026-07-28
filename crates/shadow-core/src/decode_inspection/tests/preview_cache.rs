use std::{fs, path::Path};

use shadow_cache::ContentAddressedStore;
use shadow_catalog::{CachedArtifactRole, CatalogActor};
use shadow_domain::{
    DecodeSupport, DecoderSnapshot, ImageDimensions, PreviewByteOrder, PreviewCodec,
    PreviewPayload, ProxyPayload,
};

use crate::{
    display_jpeg_fixture::DISPLAY_JPEG_BYTES, technical_observation::TechnicalObservationError,
};

use super::super::{
    DecodeInspectionActor, DecodeInspectionError, DecodeInspectionOutcome, DecodeInspectionRequest,
    DecodeInspector, PreviewCacheOutcome, fingerprint_source,
};
use super::support::{Fixture, preview_descriptor, sample_snapshot};

#[test]
fn embedded_preview_is_replaced_by_a_content_addressed_generated_proxy() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let cache_root = fixture.root.join("cache");
    let worker = DecodeInspectionActor::spawn_with_cache_profiled(
        catalog.clone(),
        PreviewInspector,
        &cache_root,
    )
    .expect("spawn profiled cached inspector");

    let outcome = worker
        .handle()
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        })
        .expect("submit inspection")
        .wait()
        .expect("complete inspection");
    assert!(matches!(
        outcome,
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::StoredGeneratedProxy { byte_len, .. },
            ..
        } if byte_len == u64::try_from(DISPLAY_JPEG_BYTES.len()).expect("test JPEG length fits")
    ));

    let artifacts = catalog
        .cached_artifacts(registered.representation_id)
        .expect("read cached artifacts");
    assert_eq!(artifacts.len(), 2);
    assert_eq!(
        catalog
            .preferred_cached_artifact(registered.representation_id)
            .expect("select preferred visual")
            .expect("generated visual exists")
            .artifact
            .role,
        CachedArtifactRole::GeneratedProxy
    );
    let store = ContentAddressedStore::open(cache_root).expect("reopen cache");
    for artifact in artifacts {
        assert_eq!(artifact.artifact.codec, PreviewCodec::Jpeg);
        let digest = shadow_cache::BlobDigest::from_bytes(artifact.artifact.blob_digest);
        assert_eq!(
            fs::read(store.resolve(digest)).expect("read cached preview"),
            DISPLAY_JPEG_BYTES
        );
    }

    let terminal = worker
        .shutdown_with_performance()
        .expect("shutdown profiled inspector");
    assert!(terminal.decode.profiled);
    assert_eq!(terminal.decode.provider_inspect.samples, 1);
    assert_eq!(terminal.decode.embedded_preview_extract.samples, 1);
    assert_eq!(terminal.decode.proxy_render.samples, 1);
    assert_eq!(terminal.decode.cache_blob_put.samples, 2);
    assert_eq!(terminal.decode.cache_artifact_catalog_commit.samples, 2);
    assert_eq!(terminal.decode.technical_submit_wait.samples, 2);
    assert!(terminal.technical.profiled);
    assert_eq!(terminal.technical.technical_observation_total.samples, 2);
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn missing_embedded_preview_falls_back_to_versioned_generated_proxy() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let cache_root = fixture.root.join("cache");
    let worker = DecodeInspectionActor::spawn_with_cache_profiled(
        catalog.clone(),
        ProxyInspector,
        &cache_root,
    )
    .expect("spawn profiled cached inspector");

    let outcome = worker
        .handle()
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        })
        .expect("submit inspection")
        .wait()
        .expect("complete inspection");
    assert!(matches!(
        outcome,
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::StoredGeneratedProxy {
                byte_len,
                dimensions: ImageDimensions {
                    width: 2_048,
                    height: 1_365
                },
                ..
            },
            ..
        } if byte_len == u64::try_from(DISPLAY_JPEG_BYTES.len()).expect("test JPEG length fits")
    ));

    let artifacts = catalog
        .cached_artifacts(registered.representation_id)
        .expect("read cached artifacts");
    assert_eq!(artifacts.len(), 1);
    assert_eq!(
        artifacts[0].artifact.role,
        CachedArtifactRole::GeneratedProxy
    );
    assert_eq!(
        artifacts[0].artifact.variant_key,
        "test-decoder:grid-jpeg-2048-q88-v1"
    );
    assert_eq!(artifacts[0].artifact.provider_preview_id, None);

    let terminal = worker
        .shutdown_with_performance()
        .expect("shutdown profiled inspector");
    assert_eq!(terminal.decode.provider_inspect.samples, 1);
    assert_eq!(terminal.decode.embedded_preview_extract.samples, 1);
    assert_eq!(terminal.decode.proxy_render.samples, 1);
    assert_eq!(terminal.decode.cache_blob_put.samples, 1);
    assert_eq!(terminal.decode.cache_artifact_catalog_commit.samples, 1);
    assert_eq!(terminal.decode.technical_submit_wait.samples, 1);
    assert_eq!(terminal.technical.technical_observation_total.samples, 1);
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn detached_observation_failure_is_reported_on_inspector_shutdown() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let worker = DecodeInspectionActor::spawn_with_cache(
        catalog,
        CorruptPreviewInspector,
        fixture.root.join("cache"),
    )
    .expect("spawn cached inspector");

    let outcome = worker
        .handle()
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        })
        .expect("submit inspection")
        .wait()
        .expect("cache corrupt JPEG before detached observation runs");
    assert!(matches!(outcome, DecodeInspectionOutcome::Recorded { .. }));
    assert!(matches!(
        worker.shutdown(),
        Err(DecodeInspectionError::TechnicalObservation(
            TechnicalObservationError::Bridge(_)
        ))
    ));
    actor.shutdown().expect("shutdown catalog");
}

#[derive(Debug, Copy, Clone)]
struct PreviewInspector;

impl DecodeInspector for PreviewInspector {
    fn provider_id(&self) -> &'static str {
        "test-decoder"
    }

    fn inspect(&mut self, _path: &Path) -> Result<DecoderSnapshot, String> {
        let mut snapshot = sample_snapshot();
        snapshot.provider.id = "test-decoder".into();
        snapshot.capabilities.embedded_previews = DecodeSupport::Available;
        snapshot.previews.push(preview_descriptor());
        Ok(snapshot)
    }

    fn extract_best_preview(&mut self, _path: &Path) -> Result<Option<PreviewPayload>, String> {
        Ok(Some(PreviewPayload {
            descriptor: preview_descriptor(),
            byte_order: PreviewByteOrder::NotApplicable,
            bytes: DISPLAY_JPEG_BYTES.to_vec(),
        }))
    }

    fn proxy_variant_key(&self) -> &'static str {
        "test-decoder:grid-jpeg-2048-q90-v2"
    }

    fn render_proxy(&mut self, _path: &Path) -> Result<Option<ProxyPayload>, String> {
        Ok(Some(ProxyPayload {
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
            codec: PreviewCodec::Jpeg,
            bits_per_channel: 8,
            channels: 3,
            bytes: DISPLAY_JPEG_BYTES.to_vec(),
        }))
    }
}

#[derive(Debug, Copy, Clone)]
struct ProxyInspector;

impl DecodeInspector for ProxyInspector {
    fn provider_id(&self) -> &'static str {
        "test-decoder"
    }

    fn proxy_variant_key(&self) -> &'static str {
        "test-decoder:grid-jpeg-2048-q88-v1"
    }

    fn inspect(&mut self, _path: &Path) -> Result<DecoderSnapshot, String> {
        let mut snapshot = sample_snapshot();
        snapshot.provider.id = "test-decoder".into();
        snapshot.capabilities.reference_rgb = DecodeSupport::Available;
        Ok(snapshot)
    }

    fn render_proxy(&mut self, _path: &Path) -> Result<Option<ProxyPayload>, String> {
        Ok(Some(ProxyPayload {
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
            codec: PreviewCodec::Jpeg,
            bits_per_channel: 8,
            channels: 3,
            bytes: DISPLAY_JPEG_BYTES.to_vec(),
        }))
    }
}

#[derive(Debug, Copy, Clone)]
struct CorruptPreviewInspector;

impl DecodeInspector for CorruptPreviewInspector {
    fn provider_id(&self) -> &'static str {
        "test-decoder"
    }

    fn inspect(&mut self, _path: &Path) -> Result<DecoderSnapshot, String> {
        let mut snapshot = sample_snapshot();
        snapshot.provider.id = "test-decoder".into();
        snapshot.capabilities.embedded_previews = DecodeSupport::Available;
        snapshot.previews.push(preview_descriptor());
        Ok(snapshot)
    }

    fn extract_best_preview(&mut self, _path: &Path) -> Result<Option<PreviewPayload>, String> {
        Ok(Some(PreviewPayload {
            descriptor: preview_descriptor(),
            byte_order: PreviewByteOrder::NotApplicable,
            bytes: b"not a JPEG".to_vec(),
        }))
    }
}

use std::{fs, path::Path, sync::Mutex};

use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CatalogActor, RecordCachedArtifact, RegisterAsset, TechnicalObservationRevision,
};
use shadow_domain::{
    DecoderSnapshot, EntityId, ImageDimensions, PhotoId, PreviewByteOrder, PreviewCodec,
    ProxyPayload, RepresentationKind,
};

use crate::{
    ScanCancellation,
    decode_inspection::{
        DecodeInspectionRequest, fingerprint_source, runtime_state::DecodeInspectionState,
        tests::support::sample_snapshot, visual_cache::cache_preview,
    },
    display_jpeg_fixture::DISPLAY_JPEG_BYTES,
    native_path::encode_location,
    technical_observation::{TechnicalObservationActor, technical_analysis_preprocessing_version},
};

use super::*;

#[derive(Default)]
struct Inspector {
    renders: usize,
    previews: usize,
}

impl DecodeInspector for Inspector {
    fn inspect(&mut self, _: &Path) -> Result<DecoderSnapshot, String> {
        Ok(sample_snapshot())
    }
    fn extract_best_preview(
        &mut self,
        _: &Path,
    ) -> Result<Option<shadow_domain::PreviewPayload>, String> {
        self.previews += 1;
        Ok(None)
    }
    fn render_proxy(&mut self, _: &Path) -> Result<Option<ProxyPayload>, String> {
        self.renders += 1;
        Ok(Some(ProxyPayload {
            bytes: DISPLAY_JPEG_BYTES.to_vec(),
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 8,
                height: 8,
            },
            bits_per_channel: 8,
            channels: 3,
        }))
    }
}

struct Fixture {
    actor: Option<CatalogActor>,
    catalog: shadow_catalog::CatalogHandle,
    cache: ContentAddressedStore,
    request: DecodeInspectionRequest,
    artifact: CachedArtifact,
    cancellation: ScanCancellation,
    root: std::path::PathBuf,
}

impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!("shadow-proxy-reuse-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).unwrap();
        let path = root.join("source.dng");
        fs::write(&path, b"unchanged original").unwrap();
        let source = fingerprint_source(&path).unwrap();
        let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).unwrap();
        let catalog = actor.handle();
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: encode_location(&path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 1,
            })
            .unwrap();
        let cache = ContentAddressedStore::open(root.join("cache")).unwrap();
        let blob = cache.put(DISPLAY_JPEG_BYTES).unwrap();
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: Inspector::default().proxy_variant_key().into(),
            generator_id: "anonymous".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: blob.digest.algorithm().into(),
            blob_digest: *blob.digest.as_bytes(),
            blob_byte_len: blob.byte_len,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 8,
                height: 8,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 123,
        };
        let fixture = Self {
            actor: Some(actor),
            catalog,
            cache,
            artifact,
            request: DecodeInspectionRequest {
                path,
                representation_id: registered.representation_id,
                expected_source: source,
            },
            cancellation: ScanCancellation::default(),
            root,
        };
        fixture.store(fixture.artifact.clone());
        fixture
    }

    fn store(&self, artifact: CachedArtifact) {
        self.catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: self.request.representation_id,
                expected_source: self.request.expected_source,
                artifact,
            })
            .unwrap();
    }

    fn context(&self) -> VisualCacheContext<'_> {
        VisualCacheContext {
            catalog: &self.catalog,
            cache: &self.cache,
            request: &self.request,
            provider_id: "anonymous",
            provider_version: "1",
            cancellation: &self.cancellation,
            technical_observer: None,
            embedded_preview_sink: None,
        }
    }

    fn run(&self, inspector: &mut Inspector) -> PreviewCacheOutcome {
        cache_preview(
            inspector,
            self.context(),
            &Mutex::new(DecodeInspectionState::default()),
            &mut DecodePerformance::default(),
        )
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        self.actor.take().unwrap().shutdown().unwrap();
        let _ = fs::remove_dir_all(&self.root);
    }
}

#[test]
fn exact_proxy_backfills_analysis_without_decoding_or_republishing() {
    let fixture = Fixture::new();
    let observer =
        TechnicalObservationActor::spawn_with_store(fixture.catalog.clone(), fixture.cache.clone())
            .unwrap();
    let handle = observer.handle();
    let mut context = fixture.context();
    context.technical_observer = Some(&handle);
    let state = Mutex::new(DecodeInspectionState::default());
    let mut inspector = Inspector::default();
    let result = cache_preview(
        &mut inspector,
        context,
        &state,
        &mut DecodePerformance::default(),
    );
    assert!(matches!(
        result,
        PreviewCacheOutcome::ReusedGeneratedProxy { .. }
    ));
    assert_eq!((inspector.renders, inspector.previews), (0, 0));
    assert_eq!(state.lock().unwrap().visual_artifacts_published, 0);
    observer.shutdown().unwrap();
    let artifacts = fixture
        .catalog
        .cached_artifacts(fixture.request.representation_id)
        .unwrap();
    assert_eq!(artifacts.len(), 1);
    assert_eq!(artifacts[0].artifact, fixture.artifact);
    assert!(
        fixture
            .catalog
            .technical_observation(
                fixture.request.representation_id,
                fixture.request.expected_source,
                &fixture.artifact,
                &TechnicalObservationRevision::current(technical_analysis_preprocessing_version()),
            )
            .unwrap()
            .is_some()
    );
}

#[test]
fn renderer_variant_provider_source_and_artifact_role_must_all_match() {
    for mismatch in [
        "provider", "version", "variant", "source", "embedded", "recipe",
    ] {
        let fixture = Fixture::new();
        let record = fixture
            .catalog
            .cached_artifacts(fixture.request.representation_id)
            .unwrap()
            .remove(0);
        fixture.catalog.invalidate_cached_artifact(&record).unwrap();
        let mut artifact = fixture.artifact.clone();
        match mismatch {
            "provider" => artifact.generator_id = "different-provider".into(),
            "version" => artifact.generator_version = "different-version".into(),
            "variant" => artifact.variant_key = "different-plan-or-size".into(),
            "embedded" => {
                artifact.role = CachedArtifactRole::EmbeddedPreview;
                artifact.provider_preview_id = Some(0);
            }
            "recipe" => {
                artifact.role = CachedArtifactRole::RecipePreview;
                artifact.recipe_snapshot_digest = Some([7; 32]);
            }
            "source" => {}
            _ => unreachable!(),
        }
        fixture.store(artifact);
        let mut context = fixture.context();
        let mut stale_request = fixture.request.clone();
        if mismatch == "source" {
            stale_request.expected_source.byte_len += 1;
            context.request = &stale_request;
        }
        assert_eq!(
            reuse_generated_proxy(
                &Inspector::default(),
                context,
                &mut DecodePerformance::default()
            ),
            None,
            "{mismatch}"
        );
        if mismatch != "source" {
            let mut inspector = Inspector::default();
            assert!(matches!(
                fixture.run(&mut inspector),
                PreviewCacheOutcome::StoredGeneratedProxy { .. }
            ));
            assert_eq!(inspector.renders, 1, "{mismatch}");
        }
    }
}

#[test]
fn missing_corrupt_and_wrong_length_blobs_are_regenerated() {
    for failure in ["missing", "corrupt", "length"] {
        let fixture = Fixture::new();
        let path = fixture
            .cache
            .resolve(BlobDigest::from_bytes(fixture.artifact.blob_digest));
        match failure {
            "missing" => fs::remove_file(&path).unwrap(),
            "corrupt" => fs::write(&path, b"damaged cache").unwrap(),
            "length" => {
                let mut artifact = fixture.artifact.clone();
                artifact.blob_byte_len += 1;
                fixture.store(artifact);
            }
            _ => unreachable!(),
        }
        let mut inspector = Inspector::default();
        assert!(
            matches!(
                fixture.run(&mut inspector),
                PreviewCacheOutcome::StoredGeneratedProxy { .. }
            ),
            "{failure}"
        );
        assert_eq!(inspector.renders, 1);
        assert_eq!(fs::read(&path).unwrap(), DISPLAY_JPEG_BYTES);
    }
}

#[test]
fn cancellation_and_filesystem_change_cannot_reuse_existing_proxy() {
    let fixture = Fixture::new();
    fixture.cancellation.cancel();
    let mut inspector = Inspector::default();
    assert_eq!(
        fixture.run(&mut inspector),
        PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
    );
    assert_eq!(inspector.renders, 0);

    let fixture = Fixture::new();
    fs::write(&fixture.request.path, b"changed original bytes").unwrap();
    assert_eq!(
        fixture.run(&mut inspector),
        PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged)
    );
    assert_eq!(inspector.renders, 0);
}

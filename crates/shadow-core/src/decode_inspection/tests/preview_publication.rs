use std::{
    path::Path,
    sync::{Arc, Mutex, mpsc},
    time::Duration,
};

use shadow_catalog::{CachedArtifactRole, CatalogActor};
use shadow_domain::{
    DecodeSupport, DecoderSnapshot, ImageDimensions, PreviewByteOrder, PreviewCodec,
    PreviewPayload, ProxyPayload,
};

use crate::display_jpeg_fixture::DISPLAY_JPEG_BYTES;

use super::super::{
    DecodeInspectionActor, DecodeInspectionOutcome, DecodeInspectionPool, DecodeInspectionProgress,
    DecodeInspectionRequest, DecodeInspectionSummary, DecodeInspector, EmbeddedPreviewPublication,
    EmbeddedPreviewSink, PreviewCacheOutcome, fingerprint_source,
};
use super::support::{Fixture, preview_descriptor, sample_snapshot};

#[test]
fn progress_publishes_embedded_preview_before_proxy_finishes() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let (proxy_started_sender, proxy_started_receiver) = mpsc::sync_channel(0);
    let (release_proxy_sender, release_proxy_receiver) = mpsc::sync_channel(0);
    let worker = DecodeInspectionActor::spawn_with_cache(
        catalog,
        BlockingProxyInspector {
            proxy_started_sender,
            release_proxy_receiver,
        },
        fixture.root.join("cache"),
    )
    .expect("spawn cached inspector");
    let handle = worker.handle();
    let ticket = handle
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        })
        .expect("submit inspection");

    proxy_started_receiver
        .recv_timeout(std::time::Duration::from_secs(1))
        .expect("embedded preview commits before proxy work blocks");
    assert_eq!(
        handle.progress_snapshot(),
        DecodeInspectionProgress {
            summary: DecodeInspectionSummary::default(),
            visual_artifacts_published: 1,
        }
    );

    release_proxy_sender
        .send(())
        .expect("release generated proxy");
    assert!(matches!(
        ticket.wait().expect("proxy finishes"),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::StoredGeneratedProxy { .. },
            ..
        }
    ));
    assert_eq!(
        handle.progress_snapshot(),
        DecodeInspectionProgress {
            summary: DecodeInspectionSummary {
                completed: 1,
                hard_failures: 0,
                preview_failures: 0,
                cancelled: 0,
            },
            visual_artifacts_published: 2,
        }
    );

    worker.shutdown().expect("shutdown inspector");
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn session_preview_sink_never_persists_the_embedded_camera_preview() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let (proxy_started_sender, proxy_started_receiver) = mpsc::sync_channel(0);
    let (release_proxy_sender, release_proxy_receiver) = mpsc::sync_channel(0);
    let sink = Arc::new(RecordingPreviewSink::default());
    let preview_sink: Arc<dyn EmbeddedPreviewSink> = sink.clone();
    let mut inspector = Some(BlockingProxyInspector {
        proxy_started_sender,
        release_proxy_receiver,
    });
    let pool = DecodeInspectionPool::spawn_with_cache_and_embedded_preview_sink(
        catalog.clone(),
        1,
        move |_| Ok::<_, String>(inspector.take().expect("one test inspector")),
        fixture.root.join("cache"),
        preview_sink,
    )
    .expect("spawn session preview pool");
    let handle = pool.handle();
    let ticket = handle
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        })
        .expect("submit inspection");

    proxy_started_receiver
        .recv_timeout(Duration::from_secs(1))
        .expect("session preview publishes before proxy blocks");
    assert_eq!(sink.publication_count(), 1);
    assert!(
        catalog
            .cached_artifacts(registered.representation_id)
            .expect("read durable artifacts before proxy")
            .is_empty()
    );
    assert_eq!(
        handle.progress_snapshot().visual_artifacts_published,
        1,
        "session visual is immediately visible even though it is not cached"
    );

    release_proxy_sender.send(()).expect("release proxy");
    assert!(matches!(
        ticket.wait().expect("proxy finishes"),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::StoredGeneratedProxy { .. },
            ..
        }
    ));
    let artifacts = catalog
        .cached_artifacts(registered.representation_id)
        .expect("read durable artifacts after proxy");
    assert_eq!(artifacts.len(), 1);
    assert_eq!(
        artifacts[0].artifact.role,
        CachedArtifactRole::GeneratedProxy
    );
    assert_eq!(handle.progress_snapshot().visual_artifacts_published, 2);

    pool.shutdown().expect("shutdown session preview pool");
    actor.shutdown().expect("shutdown catalog");
}

struct BlockingProxyInspector {
    proxy_started_sender: mpsc::SyncSender<()>,
    release_proxy_receiver: mpsc::Receiver<()>,
}

#[derive(Debug, Default)]
struct RecordingPreviewSink {
    publications: Mutex<Vec<EmbeddedPreviewPublication>>,
}

impl RecordingPreviewSink {
    fn publication_count(&self) -> usize {
        self.publications.lock().map_or(0, |items| items.len())
    }
}

impl EmbeddedPreviewSink for RecordingPreviewSink {
    fn publish_embedded_preview(
        &self,
        publication: EmbeddedPreviewPublication,
    ) -> Result<(), String> {
        self.publications
            .lock()
            .map_err(|_| "recording preview sink lock is poisoned".to_owned())?
            .push(publication);
        Ok(())
    }
}

impl DecodeInspector for BlockingProxyInspector {
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
        self.proxy_started_sender
            .send(())
            .map_err(|error| error.to_string())?;
        self.release_proxy_receiver
            .recv()
            .map_err(|error| error.to_string())?;
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

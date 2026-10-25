use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

use shadow_catalog::{CatalogActor, RegisterAsset};
use shadow_domain::{
    EntityId, ImageDimensions, PhotoId, PreviewByteOrder, PreviewCodec, PreviewPayload,
    ProxyPayload, RepresentationKind,
};

use crate::{
    ScanCancellation, display_jpeg_fixture::DISPLAY_JPEG_BYTES, native_path::encode_location,
};

use super::super::{
    fingerprint_source,
    tests::support::{preview_descriptor, sample_snapshot},
};
use super::*;

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!("shadow-progressive-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).unwrap();
        Self(root)
    }
    fn request(
        &self,
        catalog: &CatalogHandle,
        index: usize,
    ) -> super::super::DecodeInspectionRequest {
        let path = self.0.join(format!("photo-{index}.dng"));
        fs::write(&path, b"read-only-test-source").unwrap();
        let source = fingerprint_source(&path).unwrap();
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: encode_location(&path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 1,
            })
            .unwrap();
        super::super::DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path,
            expected_source: source,
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[derive(Default)]
struct Gate {
    released: Mutex<bool>,
    changed: Condvar,
    entered: AtomicUsize,
}
impl Gate {
    fn release(&self) {
        *self.released.lock().unwrap() = true;
        self.changed.notify_all();
    }
}
#[derive(Default)]
struct Sink(AtomicUsize);
impl EmbeddedPreviewSink for Sink {
    fn publish_embedded_preview(
        &self,
        _: super::super::EmbeddedPreviewPublication,
    ) -> Result<(), String> {
        self.0.fetch_add(1, Ordering::SeqCst);
        Ok(())
    }
}
struct Inspector {
    gate: Arc<Gate>,
    has_preview: bool,
}
impl DecodeInspector for Inspector {
    fn inspect(&mut self, _: &Path) -> Result<shadow_domain::DecoderSnapshot, String> {
        Ok(sample_snapshot())
    }
    fn extract_best_preview(&mut self, _: &Path) -> Result<Option<PreviewPayload>, String> {
        Ok(self.has_preview.then(|| PreviewPayload {
            descriptor: preview_descriptor(),
            byte_order: PreviewByteOrder::NotApplicable,
            bytes: DISPLAY_JPEG_BYTES.to_vec(),
        }))
    }
    fn render_proxy(&mut self, _: &Path) -> Result<Option<ProxyPayload>, String> {
        self.gate.entered.fetch_add(1, Ordering::SeqCst);
        let mut released = self.gate.released.lock().unwrap();
        while !*released {
            released = self.gate.changed.wait(released).unwrap();
        }
        Ok(Some(ProxyPayload {
            dimensions: ImageDimensions {
                width: 2,
                height: 2,
            },
            codec: PreviewCodec::Jpeg,
            bits_per_channel: 8,
            channels: 1,
            bytes: DISPLAY_JPEG_BYTES.to_vec(),
        }))
    }
}
fn wait_until(predicate: impl Fn() -> bool) {
    let started = Instant::now();
    while !predicate() && started.elapsed() < Duration::from_secs(5) {
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(
        predicate(),
        "staged worker condition must complete without releasing refinement"
    );
}

#[test]
fn repeated_refinement_reuses_the_proxy_published_while_it_was_queued() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.0.join("catalog.sqlite")).unwrap();
    let catalog = actor.handle();
    let request = fixture.request(&catalog, 0);
    let gate = Arc::new(Gate::default());
    let sink = Arc::new(Sink::default());
    let pool = ProgressiveInspectionPool::spawn(
        catalog,
        1,
        1,
        |_| {
            Ok::<_, String>(Inspector {
                gate: Arc::clone(&gate),
                has_preview: true,
            })
        },
        &fixture.0.join("cache"),
        sink.clone(),
    )
    .unwrap();
    let first = pool.handle().submit(request.clone()).unwrap();
    wait_until(|| gate.entered.load(Ordering::SeqCst) == 1);
    let second = pool.handle().submit(request.clone()).unwrap();
    wait_until(|| sink.0.load(Ordering::SeqCst) == 2);
    gate.release();
    assert!(matches!(
        first.wait().unwrap(),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::StoredGeneratedProxy { .. },
            ..
        }
    ));
    assert!(matches!(
        second.wait().unwrap(),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::ReusedGeneratedProxy { .. },
            ..
        }
    ));
    // A later request is handled by the quick worker without another placeholder
    // or refinement job. Both routes must preserve exact terminal accounting.
    assert!(matches!(
        pool.handle().submit(request).unwrap().wait().unwrap(),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::ReusedGeneratedProxy { .. },
            ..
        }
    ));
    assert_eq!(sink.0.load(Ordering::SeqCst), 2);
    assert_eq!(gate.entered.load(Ordering::SeqCst), 1);
    assert_eq!(pool.shutdown_with_summary().unwrap().completed, 3);
    actor.shutdown().unwrap();
}

#[test]
fn quick_previews_cross_the_old_queue_limit_while_refinement_is_blocked() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.0.join("catalog.sqlite")).unwrap();
    let catalog = actor.handle();
    let gate = Arc::new(Gate::default());
    let sink = Arc::new(Sink::default());
    let pool = ProgressiveInspectionPool::spawn(
        catalog.clone(),
        2,
        1,
        |_| {
            Ok::<_, String>(Inspector {
                gate: Arc::clone(&gate),
                has_preview: true,
            })
        },
        &fixture.0.join("cache"),
        sink.clone(),
    )
    .unwrap();
    let handle = pool.handle();
    let requests = (0..128)
        .map(|index| fixture.request(&catalog, index))
        .collect::<Vec<_>>();
    for request in &requests {
        drop(handle.submit(request.clone()).unwrap());
    }
    wait_until(|| sink.0.load(Ordering::SeqCst) == 128 && gate.entered.load(Ordering::SeqCst) > 0);
    assert_eq!(handle.progress_snapshot().summary.completed, 0);
    assert_eq!(handle.progress_snapshot().visual_artifacts_published, 128);
    gate.release();
    let summary = pool.shutdown_with_summary().unwrap();
    assert_eq!(summary.completed, 128);
    assert_eq!(summary.hard_failures, 0);
    assert_eq!(summary.preview_failures, 0);
    let visuals = catalog
        .preferred_cached_artifacts(
            &requests
                .iter()
                .map(|r| r.representation_id)
                .collect::<Vec<_>>(),
        )
        .unwrap();
    assert!(visuals.iter().all(Option::is_some));
    for request in requests {
        assert_eq!(fs::read(request.path).unwrap(), b"read-only-test-source");
    }
    actor.shutdown().unwrap();
}

#[test]
fn cancellation_drains_both_stages_without_publishing_queued_proxies() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.0.join("catalog.sqlite")).unwrap();
    let catalog = actor.handle();
    let gate = Arc::new(Gate::default());
    let sink = Arc::new(Sink::default());
    let pool = ProgressiveInspectionPool::spawn(
        catalog.clone(),
        1,
        1,
        |_| {
            Ok::<_, String>(Inspector {
                gate: Arc::clone(&gate),
                has_preview: true,
            })
        },
        &fixture.0.join("cache"),
        sink.clone(),
    )
    .unwrap();
    let cancellation = ScanCancellation::new();
    let requests = (0..96)
        .map(|index| fixture.request(&catalog, index))
        .collect::<Vec<_>>();
    for request in &requests {
        drop(
            pool.handle()
                .submit_with_cancellation(request.clone(), &cancellation)
                .unwrap(),
        );
    }
    wait_until(|| sink.0.load(Ordering::SeqCst) == 96 && gate.entered.load(Ordering::SeqCst) > 0);
    cancellation.cancel();
    gate.release();
    let summary = pool.shutdown_with_summary().unwrap();
    assert_eq!(summary.completed, 96);
    assert_eq!(summary.cancelled, 96);
    assert_eq!(gate.entered.load(Ordering::SeqCst), 1);
    assert!(
        catalog
            .preferred_cached_artifacts(
                &requests
                    .iter()
                    .map(|r| r.representation_id)
                    .collect::<Vec<_>>()
            )
            .unwrap()
            .iter()
            .all(Option::is_none)
    );
    actor.shutdown().unwrap();
}

#[test]
fn source_replacement_between_quick_preview_and_refinement_cannot_publish() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.0.join("catalog.sqlite")).unwrap();
    let catalog = actor.handle();
    let gate = Arc::new(Gate::default());
    let pool = ProgressiveInspectionPool::spawn(
        catalog.clone(),
        1,
        1,
        |_| {
            Ok::<_, String>(Inspector {
                gate: Arc::clone(&gate),
                has_preview: false,
            })
        },
        &fixture.0.join("cache"),
        Arc::new(Sink::default()),
    )
    .unwrap();
    let request = fixture.request(&catalog, 0);
    let ticket = pool.handle().submit(request.clone()).unwrap();
    wait_until(|| gate.entered.load(Ordering::SeqCst) == 1);
    fs::write(&request.path, b"replacement-source-has-another-length").unwrap();
    gate.release();
    let outcome = ticket.wait().unwrap();
    assert!(matches!(
        outcome,
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::Discarded(
                super::super::DecodeInspectionDiscardReason::FilesystemChanged
            ),
            ..
        }
    ));
    assert_eq!(pool.shutdown_with_summary().unwrap().completed, 1);
    assert!(
        catalog
            .preferred_cached_artifacts(&[request.representation_id])
            .unwrap()[0]
            .is_none()
    );
    actor.shutdown().unwrap();
}

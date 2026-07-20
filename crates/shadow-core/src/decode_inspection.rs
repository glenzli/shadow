use std::{
    path::{Path, PathBuf},
    sync::{
        Arc,
        mpsc::{self, Receiver, Sender, SyncSender},
    },
    thread::{self, JoinHandle},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_cache::{CacheError, ContentAddressedStore, StoredBlob};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogError, CatalogHandle, RecordCachedArtifact,
    RecordCachedArtifactStatus, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};
use shadow_domain::{
    DecoderSnapshot, ImageDimensions, PreviewByteOrder, PreviewPayload, ProxyPayload,
    RepresentationId,
};
use thiserror::Error;

use crate::technical_observation::{
    TechnicalObservationActor, TechnicalObservationError, TechnicalObservationHandle,
    technical_analysis_preprocessing_version,
};

const INSPECTION_QUEUE_CAPACITY: usize = 32;

/// Provider-neutral operation used by the background decode worker.
///
/// Implementations adapt `LibRaw`, a private vendor SDK bridge, a DNG converter,
/// or a test double without exposing provider types to the scheduler.
pub trait DecodeInspector: Send + 'static {
    /// Stable provider id used for Catalog cache reconciliation.
    #[allow(clippy::unnecessary_literal_bound)]
    fn provider_id(&self) -> &str {
        "anonymous"
    }

    /// Provider build/version used to invalidate stale capability results.
    #[allow(clippy::unnecessary_literal_bound)]
    fn provider_version(&self) -> &str {
        "1"
    }

    /// Inspects one source and returns an owned, provider-neutral snapshot.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic suitable for the background job log.
    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String>;

    /// Extracts the provider-selected embedded preview when available.
    ///
    /// The default keeps descriptor-only inspectors valid. Implementations
    /// should return `Ok(None)` for a legitimate no-preview source.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic when preview extraction fails.
    fn extract_best_preview(&mut self, _path: &Path) -> Result<Option<PreviewPayload>, String> {
        Ok(None)
    }

    /// Renders a bounded display proxy when no embedded preview is available.
    ///
    /// The variant key is stored in the Catalog so later renderer changes can
    /// invalidate only proxies produced by an older recipe.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic when reference rendering or encoding fails.
    fn render_proxy(&mut self, _path: &Path) -> Result<Option<ProxyPayload>, String> {
        Ok(None)
    }

    #[allow(clippy::unnecessary_literal_bound)]
    fn proxy_variant_key(&self) -> &str {
        "anonymous:grid-jpeg-2048-q88-v1"
    }
}

impl<F> DecodeInspector for F
where
    F: FnMut(&Path) -> Result<DecoderSnapshot, String> + Send + 'static,
{
    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        self(path)
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct DecodeInspectionRequest {
    pub representation_id: RepresentationId,
    pub path: PathBuf,
    pub expected_source: RepresentationFingerprint,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum DecodeInspectionDiscardReason {
    FilesystemChanged,
    CatalogChanged,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub enum DecodeInspectionOutcome {
    Recorded {
        provider_id: String,
        provider_version: String,
        preview: PreviewCacheOutcome,
    },
    Discarded(DecodeInspectionDiscardReason),
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub enum PreviewCacheOutcome {
    NotRequested,
    NoVisualAvailable,
    StoredEmbeddedPreview {
        digest_hex: String,
        byte_len: u64,
    },
    StoredGeneratedProxy {
        digest_hex: String,
        byte_len: u64,
        dimensions: ImageDimensions,
    },
    Discarded(DecodeInspectionDiscardReason),
    Failed(String),
}

#[derive(Debug, Error)]
pub enum DecodeInspectionError {
    #[error("cannot read source metadata for {path}: {source}")]
    SourceMetadata {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("decoder inspection failed for {path}: {message}")]
    Inspector { path: PathBuf, message: String },
    #[error("catalog operation failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("preview cache operation failed: {0}")]
    Cache(#[from] CacheError),
    #[error("technical observation operation failed: {0}")]
    TechnicalObservation(#[from] TechnicalObservationError),
    #[error("cannot start decode inspection worker: {0}")]
    WorkerStart(#[source] std::io::Error),
    #[error("decode inspection worker is unavailable")]
    WorkerUnavailable,
    #[error("decode inspection did not complete within {0:?}")]
    CompletionTimeout(std::time::Duration),
    #[error("decode inspection worker panicked")]
    WorkerPanicked,
}

#[derive(Debug)]
pub struct DecodeInspectionActor {
    handle: DecodeInspectionHandle,
    join_handle: Option<JoinHandle<()>>,
    technical_observer: Option<TechnicalObservationActor>,
}

#[derive(Debug, Clone)]
pub struct DecodeInspectionHandle {
    sender: SyncSender<Message>,
    provider_id: Arc<str>,
    provider_version: Arc<str>,
    proxy_variant_key: Arc<str>,
    technical_preprocessing_version: Option<Arc<str>>,
    caches_previews: bool,
}

#[derive(Debug)]
pub struct DecodeInspectionTicket {
    receiver: Receiver<Result<DecodeInspectionOutcome, DecodeInspectionError>>,
}

enum Message {
    Inspect(
        DecodeInspectionRequest,
        Sender<Result<DecodeInspectionOutcome, DecodeInspectionError>>,
    ),
    Shutdown(SyncSender<()>),
}

impl DecodeInspectionActor {
    /// Starts one bounded, serialized decode-inspection queue.
    ///
    /// The decoder runs on this worker, while all `SQLite` writes are forwarded
    /// to [`CatalogHandle`]'s dedicated writer thread. Folder scanning therefore
    /// performs neither expensive RAW decoding nor direct database writes.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::WorkerStart`] when the worker thread
    /// cannot be created.
    pub fn spawn(
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
    ) -> Result<Self, DecodeInspectionError> {
        Self::spawn_inner(catalog, inspector, None)
    }

    /// Starts a decode worker that also writes selected embedded previews into
    /// a content-addressed cache root.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError`] when the cache root or worker cannot be
    /// initialized.
    pub fn spawn_with_cache(
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, DecodeInspectionError> {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_inner(catalog, inspector, Some(cache))
    }

    fn spawn_inner(
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
        cache: Option<ContentAddressedStore>,
    ) -> Result<Self, DecodeInspectionError> {
        let provider_id = Arc::<str>::from(inspector.provider_id());
        let provider_version = Arc::<str>::from(inspector.provider_version());
        let proxy_variant_key = Arc::<str>::from(inspector.proxy_variant_key());
        let caches_previews = cache.is_some();
        let technical_observer = cache
            .as_ref()
            .map(|cache| {
                TechnicalObservationActor::spawn_with_store(catalog.clone(), cache.clone())
            })
            .transpose()?;
        let technical_handle = technical_observer
            .as_ref()
            .map(TechnicalObservationActor::handle);
        let technical_preprocessing_version = technical_handle
            .as_ref()
            .map(|_| Arc::<str>::from(technical_analysis_preprocessing_version()));
        let (sender, receiver) = mpsc::sync_channel(INSPECTION_QUEUE_CAPACITY);
        let join_handle = thread::Builder::new()
            .name("shadow-decode-inspector".to_owned())
            .spawn(move || {
                run_worker(
                    &catalog,
                    inspector,
                    cache.as_ref(),
                    technical_handle.as_ref(),
                    &receiver,
                );
            })
            .map_err(DecodeInspectionError::WorkerStart)?;
        Ok(Self {
            handle: DecodeInspectionHandle {
                sender,
                provider_id,
                provider_version,
                proxy_variant_key,
                technical_preprocessing_version,
                caches_previews,
            },
            join_handle: Some(join_handle),
            technical_observer,
        })
    }

    pub fn handle(&self) -> DecodeInspectionHandle {
        self.handle.clone()
    }

    /// Drains previously submitted work, then stops the worker.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError`] if the worker disconnected or panicked.
    pub fn shutdown(mut self) -> Result<(), DecodeInspectionError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<(), DecodeInspectionError> {
        let decode_result = self.join_handle.take().map_or(Ok(()), |join_handle| {
            let (response_sender, response_receiver) = mpsc::sync_channel(0);
            if self
                .handle
                .sender
                .send(Message::Shutdown(response_sender))
                .is_err()
            {
                return match join_handle.join() {
                    Ok(()) => Err(DecodeInspectionError::WorkerUnavailable),
                    Err(_) => Err(DecodeInspectionError::WorkerPanicked),
                };
            }
            let acknowledged = response_receiver.recv();
            if join_handle.join().is_err() {
                return Err(DecodeInspectionError::WorkerPanicked);
            }
            acknowledged.map_err(|_| DecodeInspectionError::WorkerUnavailable)
        });
        let observation_result = self
            .technical_observer
            .take()
            .map(TechnicalObservationActor::shutdown)
            .transpose();
        decode_result?;
        observation_result?;
        Ok(())
    }
}

impl Drop for DecodeInspectionActor {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

impl DecodeInspectionHandle {
    pub fn provider_id(&self) -> &str {
        &self.provider_id
    }

    pub fn provider_version(&self) -> &str {
        &self.provider_version
    }

    pub fn proxy_variant_key(&self) -> &str {
        &self.proxy_variant_key
    }

    pub const fn caches_previews(&self) -> bool {
        self.caches_previews
    }

    pub fn technical_preprocessing_version(&self) -> Option<&str> {
        self.technical_preprocessing_version.as_deref()
    }

    /// Queues an inspection and returns immediately with a completion ticket.
    ///
    /// The bounded queue applies backpressure when imports outrun decoding.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::WorkerUnavailable`] when the worker has
    /// already stopped.
    pub fn submit(
        &self,
        request: DecodeInspectionRequest,
    ) -> Result<DecodeInspectionTicket, DecodeInspectionError> {
        let (response_sender, response_receiver) = mpsc::channel();
        self.sender
            .send(Message::Inspect(request, response_sender))
            .map_err(|_| DecodeInspectionError::WorkerUnavailable)?;
        Ok(DecodeInspectionTicket {
            receiver: response_receiver,
        })
    }
}

impl DecodeInspectionTicket {
    /// Waits for this inspection without stopping the worker or later jobs.
    ///
    /// # Errors
    ///
    /// Returns the provider, filesystem, or catalog error produced by the job,
    /// or [`DecodeInspectionError::WorkerUnavailable`] after a disconnect.
    pub fn wait(self) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
        self.receiver
            .recv()
            .map_err(|_| DecodeInspectionError::WorkerUnavailable)?
    }

    /// Waits at most `timeout` for this inspection.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::CompletionTimeout`] when the deadline
    /// expires, or the same job and disconnect errors as [`Self::wait`].
    pub fn wait_timeout(
        self,
        timeout: std::time::Duration,
    ) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
        self.receiver
            .recv_timeout(timeout)
            .map_err(|error| match error {
                mpsc::RecvTimeoutError::Timeout => {
                    DecodeInspectionError::CompletionTimeout(timeout)
                }
                mpsc::RecvTimeoutError::Disconnected => DecodeInspectionError::WorkerUnavailable,
            })?
    }
}

/// Reads the size and native modification time used to guard an inspection.
///
/// # Errors
///
/// Returns the filesystem metadata error for an absent or inaccessible path.
pub fn fingerprint_source(path: &Path) -> Result<RepresentationFingerprint, std::io::Error> {
    let metadata = path.metadata()?;
    Ok(RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    })
}

fn run_worker(
    catalog: &CatalogHandle,
    mut inspector: impl DecodeInspector,
    cache: Option<&ContentAddressedStore>,
    technical_observer: Option<&TechnicalObservationHandle>,
    receiver: &Receiver<Message>,
) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::Inspect(request, response) => {
                let result = inspect_and_record(
                    catalog,
                    &mut inspector,
                    cache,
                    technical_observer,
                    &request,
                );
                let _ = response.send(result);
            }
            Message::Shutdown(response) => {
                let _ = response.send(());
                break;
            }
        }
    }
}

fn inspect_and_record(
    catalog: &CatalogHandle,
    inspector: &mut impl DecodeInspector,
    cache: Option<&ContentAddressedStore>,
    technical_observer: Option<&TechnicalObservationHandle>,
    request: &DecodeInspectionRequest,
) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
    if read_source_fingerprint(&request.path)? != request.expected_source {
        return Ok(DecodeInspectionOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }

    let snapshot =
        inspector
            .inspect(&request.path)
            .map_err(|message| DecodeInspectionError::Inspector {
                path: request.path.clone(),
                message,
            })?;
    if snapshot.provider.id != inspector.provider_id()
        || snapshot.provider.version != inspector.provider_version()
    {
        return Err(DecodeInspectionError::Inspector {
            path: request.path.clone(),
            message: format!(
                "inspector provider {}/{} disagrees with snapshot provider {}/{}",
                inspector.provider_id(),
                inspector.provider_version(),
                snapshot.provider.id,
                snapshot.provider.version
            ),
        });
    }

    if read_source_fingerprint(&request.path)? != request.expected_source {
        return Ok(DecodeInspectionOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }

    let provider_id = snapshot.provider.id.clone();
    let provider_version = snapshot.provider.version.clone();
    let status = catalog.record_decode_snapshot(&RecordDecodeSnapshot {
        representation_id: request.representation_id,
        expected_source: request.expected_source,
        snapshot,
        inspected_at_ms: now_ms(),
    })?;

    Ok(match status {
        RecordDecodeSnapshotStatus::Recorded => {
            let preview = cache.map_or(PreviewCacheOutcome::NotRequested, |cache| {
                cache_preview(
                    catalog,
                    inspector,
                    cache,
                    technical_observer,
                    request,
                    &provider_id,
                    &provider_version,
                )
            });
            DecodeInspectionOutcome::Recorded {
                provider_id,
                provider_version,
                preview,
            }
        }
        RecordDecodeSnapshotStatus::StaleSource => {
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::CatalogChanged)
        }
    })
}

fn cache_preview(
    catalog: &CatalogHandle,
    inspector: &mut impl DecodeInspector,
    cache: &ContentAddressedStore,
    technical_observer: Option<&TechnicalObservationHandle>,
    request: &DecodeInspectionRequest,
    provider_id: &str,
    provider_version: &str,
) -> PreviewCacheOutcome {
    if source_changed(request) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    let preview = match inspector.extract_best_preview(&request.path) {
        Ok(preview) => preview,
        Err(message) => return PreviewCacheOutcome::Failed(message),
    };
    if source_changed(request) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }

    let (bytes, artifact, stored_kind) = if let Some(preview) = preview {
        let artifact = CachedArtifact {
            role: CachedArtifactRole::EmbeddedPreview,
            variant_key: provider_id.to_owned(),
            generator_id: provider_id.to_owned(),
            generator_version: provider_version.to_owned(),
            provider_preview_id: Some(preview.descriptor.provider_id),
            blob_algorithm: String::new(),
            blob_digest: [0; 32],
            blob_byte_len: 0,
            codec: preview.descriptor.codec,
            byte_order: preview.byte_order,
            dimensions: preview.descriptor.dimensions,
            bits_per_channel: preview.descriptor.bits_per_channel,
            channels: preview.descriptor.channels,
            created_at_ms: 0,
        };
        (preview.bytes, artifact, CachedVisualKind::EmbeddedPreview)
    } else {
        let proxy = match inspector.render_proxy(&request.path) {
            Ok(Some(proxy)) => proxy,
            Ok(None) => return PreviewCacheOutcome::NoVisualAvailable,
            Err(message) => return PreviewCacheOutcome::Failed(message),
        };
        let dimensions = proxy.dimensions;
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: inspector.proxy_variant_key().to_owned(),
            generator_id: provider_id.to_owned(),
            generator_version: provider_version.to_owned(),
            provider_preview_id: None,
            blob_algorithm: String::new(),
            blob_digest: [0; 32],
            blob_byte_len: 0,
            codec: proxy.codec,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions,
            bits_per_channel: proxy.bits_per_channel,
            channels: proxy.channels,
            created_at_ms: 0,
        };
        (
            proxy.bytes,
            artifact,
            CachedVisualKind::GeneratedProxy(dimensions),
        )
    };
    if source_changed(request) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    let blob = match cache.put(&bytes) {
        Ok(blob) => blob,
        Err(error) => return PreviewCacheOutcome::Failed(error.to_string()),
    };
    if source_changed(request) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    let artifact = CachedArtifact {
        blob_algorithm: blob.digest.algorithm().to_owned(),
        blob_digest: *blob.digest.as_bytes(),
        blob_byte_len: blob.byte_len,
        created_at_ms: now_ms(),
        ..artifact
    };
    record_cached_visual(
        catalog,
        technical_observer,
        request,
        artifact,
        stored_kind,
        &blob,
    )
}

fn record_cached_visual(
    catalog: &CatalogHandle,
    technical_observer: Option<&TechnicalObservationHandle>,
    request: &DecodeInspectionRequest,
    artifact: CachedArtifact,
    stored_kind: CachedVisualKind,
    blob: &StoredBlob,
) -> PreviewCacheOutcome {
    let status = catalog.record_cached_artifact(&RecordCachedArtifact {
        representation_id: request.representation_id,
        expected_source: request.expected_source,
        artifact,
    });
    match status {
        Ok(RecordCachedArtifactStatus::Recorded) => {
            if let Some(observer) = technical_observer {
                let _ = observer
                    .submit_preferred_detached(request.representation_id, request.expected_source);
            }
            match stored_kind {
                CachedVisualKind::EmbeddedPreview => PreviewCacheOutcome::StoredEmbeddedPreview {
                    digest_hex: blob.digest.to_hex(),
                    byte_len: blob.byte_len,
                },
                CachedVisualKind::GeneratedProxy(dimensions) => {
                    PreviewCacheOutcome::StoredGeneratedProxy {
                        digest_hex: blob.digest.to_hex(),
                        byte_len: blob.byte_len,
                        dimensions,
                    }
                }
            }
        }
        Ok(RecordCachedArtifactStatus::StaleSource) => {
            PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::CatalogChanged)
        }
        Err(error) => PreviewCacheOutcome::Failed(error.to_string()),
    }
}

#[derive(Debug, Copy, Clone)]
enum CachedVisualKind {
    EmbeddedPreview,
    GeneratedProxy(ImageDimensions),
}

fn source_changed(request: &DecodeInspectionRequest) -> bool {
    fingerprint_source(&request.path).map_or(true, |current| current != request.expected_source)
}

fn read_source_fingerprint(
    path: &Path,
) -> Result<RepresentationFingerprint, DecodeInspectionError> {
    fingerprint_source(path).map_err(|source| DecodeInspectionError::SourceMetadata {
        path: path.to_path_buf(),
        source,
    })
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}

fn now_ms() -> i64 {
    system_time_ms(SystemTime::now()).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use std::{fs, sync::mpsc};

    use shadow_catalog::{CatalogActor, RegisterAsset};
    use shadow_domain::{
        AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, EntityId,
        ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PhotoId, Platform,
        PreviewByteOrder, PreviewCodec, PreviewDescriptorSnapshot, RawMetadataSnapshot,
        RepresentationKind,
    };

    use crate::technical_observation::TEST_DISPLAY_JPEG;

    use super::*;

    #[test]
    fn worker_persists_snapshot_without_decoding_on_caller_or_writer_thread() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let (thread_sender, thread_receiver) = mpsc::channel();
        let worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
            thread_sender
                .send(thread::current().name().map(str::to_owned))
                .expect("report worker thread");
            Ok(sample_snapshot())
        })
        .expect("spawn inspector");

        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };
        let first_ticket = worker
            .handle()
            .submit(request.clone())
            .expect("submit first inspection");
        let second_ticket = worker
            .handle()
            .submit(request)
            .expect("submit second inspection");
        assert_eq!(
            second_ticket
                .wait_timeout(std::time::Duration::from_secs(1))
                .expect("complete second inspection without waiting for first ticket"),
            DecodeInspectionOutcome::Recorded {
                provider_id: "anonymous".into(),
                provider_version: "1".into(),
                preview: PreviewCacheOutcome::NotRequested,
            }
        );
        assert!(matches!(
            first_ticket.wait().expect("complete first inspection"),
            DecodeInspectionOutcome::Recorded { .. }
        ));
        for _ in 0..2 {
            assert_eq!(
                thread_receiver.recv().expect("worker name").as_deref(),
                Some("shadow-decode-inspector")
            );
        }
        let snapshots = catalog
            .decode_snapshots(registered.representation_id)
            .expect("read snapshot");
        assert_eq!(snapshots.len(), 1);
        assert_eq!(snapshots[0].snapshot, sample_snapshot());

        worker.shutdown().expect("shutdown inspector");
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn changed_file_is_discarded_before_provider_work() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        fs::write(&fixture.raw_path, b"changed and longer").expect("change source");
        let worker = DecodeInspectionActor::spawn(catalog.clone(), |_path: &Path| {
            panic!("provider must not run for a stale source")
        })
        .expect("spawn inspector");

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
        assert_eq!(
            outcome,
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged)
        );
        assert!(
            catalog
                .decode_snapshots(registered.representation_id)
                .expect("read snapshots")
                .is_empty()
        );

        worker.shutdown().expect("shutdown inspector");
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn embedded_preview_is_content_addressed_and_cataloged() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let cache_root = fixture.root.join("cache");
        let worker =
            DecodeInspectionActor::spawn_with_cache(catalog.clone(), PreviewInspector, &cache_root)
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
            .expect("complete inspection");
        assert!(matches!(
            outcome,
            DecodeInspectionOutcome::Recorded {
                preview: PreviewCacheOutcome::StoredEmbeddedPreview { byte_len, .. },
                ..
            } if byte_len == u64::try_from(TEST_DISPLAY_JPEG.len()).expect("test JPEG length fits")
        ));

        let artifacts = catalog
            .cached_artifacts(registered.representation_id)
            .expect("read cached artifacts");
        assert_eq!(artifacts.len(), 1);
        assert_eq!(artifacts[0].artifact.codec, PreviewCodec::Jpeg);
        let store = ContentAddressedStore::open(cache_root).expect("reopen cache");
        let digest = shadow_cache::BlobDigest::from_bytes(artifacts[0].artifact.blob_digest);
        assert_eq!(
            fs::read(store.resolve(digest)).expect("read cached preview"),
            TEST_DISPLAY_JPEG
        );

        worker.shutdown().expect("shutdown inspector");
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
        let worker =
            DecodeInspectionActor::spawn_with_cache(catalog.clone(), ProxyInspector, &cache_root)
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
            } if byte_len == u64::try_from(TEST_DISPLAY_JPEG.len()).expect("test JPEG length fits")
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

        worker.shutdown().expect("shutdown inspector");
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
                bytes: TEST_DISPLAY_JPEG.to_vec(),
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
                bytes: TEST_DISPLAY_JPEG.to_vec(),
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

    fn preview_descriptor() -> PreviewDescriptorSnapshot {
        PreviewDescriptorSnapshot {
            provider_id: 7,
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 1_600,
                height: 1_200,
            },
            bits_per_channel: 8,
            channels: 3,
            encoded_bytes: u64::try_from(TEST_DISPLAY_JPEG.len()).expect("test JPEG length fits"),
            decodable: true,
        }
    }

    #[derive(Debug)]
    struct Fixture {
        root: PathBuf,
        raw_path: PathBuf,
        database_path: PathBuf,
    }

    impl Fixture {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!("shadow-decode-{}", PhotoId::new_v7()));
            fs::create_dir_all(&root).expect("create fixture directory");
            let raw_path = root.join("input.dng");
            fs::write(&raw_path, b"fixture").expect("write fixture");
            Self {
                database_path: root.join("catalog.sqlite"),
                root,
                raw_path,
            }
        }

        fn registration(&self, source: RepresentationFingerprint) -> RegisterAsset {
            RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    self.raw_path.as_os_str().as_encoded_bytes().to_vec(),
                    self.raw_path.display().to_string(),
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            }
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.root).expect("remove fixture directory");
        }
    }

    fn sample_snapshot() -> DecoderSnapshot {
        DecoderSnapshot {
            provider: DecodeProviderSnapshot {
                id: "anonymous".into(),
                version: "1".into(),
                dng_sdk: false,
                rawspeed: false,
                jpeg: false,
            },
            metadata: RawMetadataSnapshot {
                make: "Test".into(),
                model: "Fixture".into(),
                normalized_make: "Test".into(),
                normalized_model: "Fixture".into(),
                dng_version: Some("1.4.0.0".into()),
                raw_count: 1,
                raw_dimensions: ImageDimensions {
                    width: 10,
                    height: 10,
                },
                image_dimensions: ImageDimensions {
                    width: 10,
                    height: 10,
                },
                margins: ImageMargins::default(),
                orientation: 0,
                cfa_pattern: "RGGB".into(),
                sensor_colors: 3,
                sensor_bits: 12,
                black_level: 0,
                white_level: 4_095,
                as_shot_neutral: [1.0; 4],
                baseline_exposure: 0.0,
            },
            capabilities: DecodeCapabilitySnapshot {
                metadata: DecodeSupport::Available,
                embedded_previews: DecodeSupport::Unavailable,
                mosaic: DecodeSupport::Available,
                reference_rgb: DecodeSupport::Unavailable,
                pending_corrections: PendingCorrectionsSnapshot::default(),
            },
            previews: Vec::new(),
        }
    }
}

use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
        mpsc::{self, Receiver, Sender, SyncSender},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use shadow_cache::{CacheError, ContentAddressedStore, StoredBlob};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogError, CatalogHandle, RecordCachedArtifact,
    RecordCachedArtifactStatus, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};
use shadow_domain::{
    DecoderSnapshot, ImageDimensions, PreviewByteOrder, PreviewPayload, ProxyPayload,
    RepresentationId, RepresentationKind,
};
use thiserror::Error;

use crate::import::ScanCancellation;
use crate::performance::{DecodePerformance, DurationStats, TechnicalPerformance, measure_if};
use crate::technical_observation::{
    TechnicalObservationActor, TechnicalObservationError, TechnicalObservationHandle,
    technical_analysis_preprocessing_version,
};

const INSPECTION_QUEUE_CAPACITY: usize = 32;
const MIN_RECOMMENDED_INSPECTION_WORKERS: usize = 2;
const MAX_RECOMMENDED_INSPECTION_WORKERS: usize = 4;
const LOGICAL_CPUS_PER_INSPECTION_WORKER: usize = 6;

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

    /// Returns whether this inspector can safely inspect an original RAW
    /// source.
    ///
    /// The conservative default is RAW-only. This preserves the current
    /// LibRaw-backed behavior. Raster formats use the separate, extension
    /// precise [`Self::supported_original_raster_extensions`] contract so an
    /// inspector that only understands JPEG is never sent a TIFF or PNG.
    fn supports_original_raw(&self) -> bool {
        true
    }

    /// Returns the lower-level raster file extensions this inspector can
    /// actually open, without the leading period.
    ///
    /// The empty default is deliberate: raster files remain discoverable in
    /// the catalog, but they are not submitted to a RAW-only inspector. A
    /// provider may report `jpg`/`jpeg`, and conditionally `heic`/`heif` when
    /// its optional HEIF backend is compiled in.
    fn supported_original_raster_extensions(&self) -> Vec<String> {
        Vec::new()
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
        "anonymous:grid-jpeg-2048-q95-444-v1"
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
    Cancelled,
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
    /// An embedded camera preview was handed to a session-owned visual sink.
    ///
    /// Unlike [`Self::StoredEmbeddedPreview`], this result intentionally has
    /// no cache blob or Catalog artifact. It is valid only while that desktop
    /// session keeps the preview in memory.
    PublishedEmbeddedPreview {
        byte_len: u64,
    },
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

/// Terminal accounting for every inspection accepted by one actor.
///
/// `completed` is the total number of accepted requests that reached a
/// terminal result. The remaining counters are diagnostic subsets of that
/// total: hard worker results, preview-only failures, and cooperative
/// cancellation respectively.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq)]
pub struct DecodeInspectionSummary {
    pub completed: u64,
    pub hard_failures: u64,
    pub preview_failures: u64,
    pub cancelled: u64,
}

/// A non-terminal observation of a decode inspection pool.
///
/// `summary` preserves the exact terminal accounting contract: a request only
/// contributes to it after its complete inspection has finished.  Visual
/// publications are deliberately separate because an embedded camera preview
/// may become usable before the worker has finished generating Shadow's proxy.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq)]
pub struct DecodeInspectionProgress {
    pub summary: DecodeInspectionSummary,
    pub visual_artifacts_published: u64,
}

/// One session-scoped embedded preview publication.
///
/// The decode scheduler owns extraction timing, while the consumer owns
/// retention. This prevents camera-produced previews from becoming durable
/// Shadow cache artifacts while still allowing a Library card to appear
/// before deterministic proxy rendering has finished.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EmbeddedPreviewPublication {
    pub representation_id: RepresentationId,
    pub source: RepresentationFingerprint,
    pub preview: PreviewPayload,
}

/// Receives transient camera previews for the lifetime of a host session.
///
/// Implementations must not assume that the source will remain valid after
/// publication; consumers should use `representation_id` plus `source` as
/// their identity guard. Returning an error discards only this temporary
/// visual and never prevents generated-proxy rendering from continuing.
pub trait EmbeddedPreviewSink: Send + Sync {
    /// Publishes one embedded preview without creating a durable cache entry.
    fn publish_embedded_preview(
        &self,
        publication: EmbeddedPreviewPublication,
    ) -> Result<(), String>;
}

/// Terminal accounting and optional phase aggregates for one decode actor.
///
/// Actors created with the default constructors return disabled, empty
/// performance aggregates while preserving the exact summary contract.
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct DecodeInspectionTerminal {
    pub summary: DecodeInspectionSummary,
    pub decode: DecodePerformance,
    pub technical: TechnicalPerformance,
}

impl DecodeInspectionSummary {
    fn record(&mut self, result: &Result<DecodeInspectionOutcome, DecodeInspectionError>) {
        self.completed = self.completed.saturating_add(1);
        match result {
            Err(_) => self.hard_failures = self.hard_failures.saturating_add(1),
            Ok(
                DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
                | DecodeInspectionOutcome::Recorded {
                    preview:
                        PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled),
                    ..
                },
            ) => self.cancelled = self.cancelled.saturating_add(1),
            Ok(DecodeInspectionOutcome::Recorded {
                preview: PreviewCacheOutcome::Failed(_),
                ..
            }) => self.preview_failures = self.preview_failures.saturating_add(1),
            Ok(_) => {}
        }
    }
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
    #[error("decode inspection pool requires at least one worker")]
    InvalidWorkerCount,
    #[error("cannot construct decode inspection worker {worker_index}: {message}")]
    WorkerFactory {
        worker_index: usize,
        message: String,
    },
    #[error("decode inspection worker {worker_index} has a different provider contract")]
    WorkerContractMismatch { worker_index: usize },
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
    join_handles: Vec<JoinHandle<()>>,
    technical_observer: Option<TechnicalObservationActor>,
    profiled: bool,
}

/// Bounded set of independent decode inspectors sharing one submission handle.
///
/// Each worker owns its inspector. Provider implementations therefore retain
/// their native session isolation instead of being hidden behind a process-wide
/// mutex. The queue holds only request metadata; decoded pixels stay bounded by
/// the worker count.
#[derive(Debug)]
pub struct DecodeInspectionPool {
    actor: DecodeInspectionActor,
    worker_count: usize,
}

#[derive(Debug, Clone)]
pub struct DecodeInspectionHandle {
    senders: Arc<[SyncSender<Message>]>,
    next_worker: Arc<AtomicUsize>,
    state: Arc<Mutex<DecodeInspectionState>>,
    provider_id: Arc<str>,
    provider_version: Arc<str>,
    proxy_variant_key: Arc<str>,
    technical_preprocessing_version: Option<Arc<str>>,
    supports_original_raw: bool,
    supported_original_raster_extensions: Arc<[String]>,
    caches_previews: bool,
    profiled: bool,
}

#[derive(Debug)]
pub struct DecodeInspectionTicket {
    receiver: Receiver<Result<DecodeInspectionOutcome, DecodeInspectionError>>,
}

#[derive(Debug, Default)]
struct DecodeInspectionState {
    stopping: bool,
    worker_panicked: bool,
    summary: DecodeInspectionSummary,
    visual_artifacts_published: u64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
struct DecodeInspectorContract {
    provider_id: Arc<str>,
    provider_version: Arc<str>,
    proxy_variant_key: Arc<str>,
    supports_original_raw: bool,
    supported_original_raster_extensions: Arc<[String]>,
}

enum Message {
    Inspect(InspectionMessage),
    Shutdown(SyncSender<DecodePerformance>),
}

struct InspectionMessage {
    request: DecodeInspectionRequest,
    cancellation: ScanCancellation,
    response: Sender<Result<DecodeInspectionOutcome, DecodeInspectionError>>,
    enqueued_at: Option<Instant>,
}

type SpawnedInspectionThreads = (Arc<[SyncSender<Message>]>, Vec<JoinHandle<()>>);

pub(crate) struct DecodeInspectionSubmission {
    pub ticket: DecodeInspectionTicket,
    pub queue_full_events: u64,
}

fn decode_inspector_contract(inspector: &impl DecodeInspector) -> DecodeInspectorContract {
    let mut supported_original_raster_extensions = inspector
        .supported_original_raster_extensions()
        .into_iter()
        .map(|extension| {
            extension
                .trim()
                .trim_start_matches('.')
                .to_ascii_lowercase()
        })
        .filter(|extension| !extension.is_empty())
        .collect::<Vec<_>>();
    supported_original_raster_extensions.sort_unstable();
    supported_original_raster_extensions.dedup();
    DecodeInspectorContract {
        provider_id: Arc::from(inspector.provider_id()),
        provider_version: Arc::from(inspector.provider_version()),
        proxy_variant_key: Arc::from(inspector.proxy_variant_key()),
        // Only directly imported originals participate in the folder scanner.
        // Derived representations remain producer-owned artifacts.
        supports_original_raw: inspector.supports_original_raw(),
        supported_original_raster_extensions: supported_original_raster_extensions.into(),
    }
}

fn drain_and_join_started_workers(
    senders: &[SyncSender<Message>],
    join_handles: &mut Vec<JoinHandle<()>>,
) {
    let mut acknowledgements = Vec::with_capacity(senders.len());
    for sender in senders {
        let (response_sender, response_receiver) = mpsc::sync_channel(1);
        if sender.send(Message::Shutdown(response_sender)).is_ok() {
            acknowledgements.push(response_receiver);
        }
    }
    for acknowledgement in acknowledgements {
        let _ = acknowledgement.recv();
    }
    for join_handle in join_handles.drain(..) {
        let _ = join_handle.join();
    }
}

fn merge_duration_stats(target: &mut DurationStats, source: DurationStats) {
    target.samples = target.samples.saturating_add(source.samples);
    target.total_ns = target.total_ns.saturating_add(source.total_ns);
    target.max_ns = target.max_ns.max(source.max_ns);
}

fn merge_decode_performance(target: &mut DecodePerformance, source: &DecodePerformance) {
    target.profiled |= source.profiled;
    merge_duration_stats(&mut target.queue_wait, source.queue_wait);
    merge_duration_stats(&mut target.source_guard_stat, source.source_guard_stat);
    merge_duration_stats(&mut target.provider_inspect, source.provider_inspect);
    merge_duration_stats(
        &mut target.snapshot_catalog_commit,
        source.snapshot_catalog_commit,
    );
    merge_duration_stats(
        &mut target.embedded_preview_extract,
        source.embedded_preview_extract,
    );
    merge_duration_stats(&mut target.proxy_render, source.proxy_render);
    merge_duration_stats(&mut target.cache_blob_put, source.cache_blob_put);
    merge_duration_stats(
        &mut target.cache_artifact_catalog_commit,
        source.cache_artifact_catalog_commit,
    );
    merge_duration_stats(
        &mut target.technical_submit_wait,
        source.technical_submit_wait,
    );
}

fn spawn_inspection_threads<I: DecodeInspector>(
    catalog: &CatalogHandle,
    inspectors: Vec<I>,
    cache: Option<&ContentAddressedStore>,
    technical_handle: Option<&TechnicalObservationHandle>,
    embedded_preview_sink: Option<&Arc<dyn EmbeddedPreviewSink>>,
    state: &Arc<Mutex<DecodeInspectionState>>,
    profiled: bool,
    queue_capacity: usize,
) -> Result<SpawnedInspectionThreads, DecodeInspectionError> {
    let worker_count = inspectors.len();
    let base_queue_capacity = queue_capacity / worker_count;
    let workers_with_extra_slot = queue_capacity % worker_count;
    let mut senders = Vec::with_capacity(worker_count);
    let mut join_handles = Vec::with_capacity(worker_count);
    for (worker_index, inspector) in inspectors.into_iter().enumerate() {
        let worker_queue_capacity =
            base_queue_capacity + usize::from(worker_index < workers_with_extra_slot);
        let (sender, receiver) = mpsc::sync_channel(worker_queue_capacity);
        let worker_catalog = catalog.clone();
        let worker_cache = cache.cloned();
        let worker_technical_handle = technical_handle.cloned();
        let worker_embedded_preview_sink = embedded_preview_sink.cloned();
        let worker_state = Arc::clone(state);
        let worker_name = if worker_count == 1 {
            "shadow-decode-inspector".to_owned()
        } else {
            format!("shadow-decode-inspector-{worker_index}")
        };
        let spawn_result = thread::Builder::new().name(worker_name).spawn(move || {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                run_worker(
                    &worker_catalog,
                    inspector,
                    worker_cache.as_ref(),
                    worker_technical_handle.as_ref(),
                    worker_embedded_preview_sink.as_deref(),
                    &receiver,
                    &worker_state,
                    profiled,
                );
            }));
            if result.is_err() {
                let mut state = worker_state
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                state.worker_panicked = true;
                state.stopping = true;
            }
        });
        let join_handle = match spawn_result {
            Ok(join_handle) => join_handle,
            Err(error) => {
                state
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .stopping = true;
                drain_and_join_started_workers(&senders, &mut join_handles);
                return Err(DecodeInspectionError::WorkerStart(error));
            }
        };
        senders.push(sender);
        join_handles.push(join_handle);
    }
    Ok((senders.into(), join_handles))
}

/// Returns a memory-conscious default for bulk source inspection.
///
/// RAW decoding may temporarily retain a full sensor buffer while the shared
/// C++ row scheduler is also active. One inspection worker is therefore
/// budgeted per six logical CPUs and the result is capped at four. The lower
/// bound of two keeps filesystem and provider latency overlapped on ordinary
/// desktop systems.
#[must_use]
pub fn recommended_decode_inspection_worker_count() -> usize {
    thread::available_parallelism()
        .map_or(MIN_RECOMMENDED_INSPECTION_WORKERS, std::num::NonZero::get)
        .div_ceil(LOGICAL_CPUS_PER_INSPECTION_WORKER)
        .clamp(
            MIN_RECOMMENDED_INSPECTION_WORKERS,
            MAX_RECOMMENDED_INSPECTION_WORKERS,
        )
}

// CatalogHandle is an inexpensive actor handle. Keep the established
// ownership-taking constructors source-compatible even though workers clone
// the handle internally.
#[allow(clippy::needless_pass_by_value)]
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
        Self::spawn_inner(&catalog, inspector, None, false)
    }

    /// Starts a decode worker with monotonic phase profiling enabled.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::WorkerStart`] when the worker thread
    /// cannot be created.
    pub fn spawn_profiled(
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
    ) -> Result<Self, DecodeInspectionError> {
        Self::spawn_inner(&catalog, inspector, None, true)
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
        Self::spawn_inner(&catalog, inspector, Some(&cache), false)
    }

    /// Starts a cached decode worker with monotonic decode and technical-
    /// observation phase profiling enabled.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError`] when the cache root or either worker
    /// cannot be initialized.
    pub fn spawn_with_cache_profiled(
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, DecodeInspectionError> {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_inner(&catalog, inspector, Some(&cache), true)
    }

    fn spawn_inner(
        catalog: &CatalogHandle,
        inspector: impl DecodeInspector,
        cache: Option<&ContentAddressedStore>,
        profiled: bool,
    ) -> Result<Self, DecodeInspectionError> {
        Self::spawn_inner_with_capacity(
            catalog,
            inspector,
            cache,
            profiled,
            INSPECTION_QUEUE_CAPACITY,
        )
    }

    fn spawn_inner_with_capacity(
        catalog: &CatalogHandle,
        inspector: impl DecodeInspector,
        cache: Option<&ContentAddressedStore>,
        profiled: bool,
        queue_capacity: usize,
    ) -> Result<Self, DecodeInspectionError> {
        Self::spawn_many_inner(
            catalog,
            vec![inspector],
            cache,
            None,
            profiled,
            queue_capacity,
        )
    }

    fn spawn_many_inner<I: DecodeInspector>(
        catalog: &CatalogHandle,
        inspectors: Vec<I>,
        cache: Option<&ContentAddressedStore>,
        embedded_preview_sink: Option<Arc<dyn EmbeddedPreviewSink>>,
        profiled: bool,
        queue_capacity: usize,
    ) -> Result<Self, DecodeInspectionError> {
        let Some(first_inspector) = inspectors.first() else {
            return Err(DecodeInspectionError::InvalidWorkerCount);
        };
        let contract = decode_inspector_contract(first_inspector);
        for (worker_index, inspector) in inspectors.iter().enumerate().skip(1) {
            if decode_inspector_contract(inspector) != contract {
                return Err(DecodeInspectionError::WorkerContractMismatch { worker_index });
            }
        }
        let caches_previews = cache.is_some();
        let mut technical_observer = cache
            .map(|cache| {
                if profiled {
                    TechnicalObservationActor::spawn_with_store_profiled(
                        catalog.clone(),
                        cache.clone(),
                    )
                } else {
                    TechnicalObservationActor::spawn_with_store(catalog.clone(), cache.clone())
                }
            })
            .transpose()?;
        let technical_handle = technical_observer
            .as_ref()
            .map(TechnicalObservationActor::handle);
        let technical_preprocessing_version = technical_handle
            .as_ref()
            .map(|_| Arc::<str>::from(technical_analysis_preprocessing_version()));
        let state = Arc::new(Mutex::new(DecodeInspectionState::default()));
        let (senders, join_handles) = match spawn_inspection_threads(
            catalog,
            inspectors,
            cache,
            technical_handle.as_ref(),
            embedded_preview_sink.as_ref(),
            &state,
            profiled,
            queue_capacity,
        ) {
            Ok(workers) => workers,
            Err(error) => {
                if let Some(observer) = technical_observer.take() {
                    let _ = observer.shutdown();
                }
                return Err(error);
            }
        };
        Ok(Self {
            handle: DecodeInspectionHandle {
                senders,
                next_worker: Arc::new(AtomicUsize::new(0)),
                state,
                provider_id: contract.provider_id,
                provider_version: contract.provider_version,
                proxy_variant_key: contract.proxy_variant_key,
                technical_preprocessing_version,
                supports_original_raw: contract.supports_original_raw,
                supported_original_raster_extensions: contract.supported_original_raster_extensions,
                caches_previews,
                profiled,
            },
            join_handles,
            technical_observer,
            profiled,
        })
    }

    pub fn handle(&self) -> DecodeInspectionHandle {
        self.handle.clone()
    }

    /// Drains submitted work, then stops the worker.
    ///
    /// Jobs whose shared scan token is already cancelled skip provider work,
    /// so shutting down after a cancelled scan drains the queue quickly while
    /// a completed scan still finishes its queued previews.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError`] if the worker disconnected or panicked.
    pub fn shutdown(mut self) -> Result<(), DecodeInspectionError> {
        self.stop_and_join().map(|_| ())
    }

    /// Drains submitted work, stops the worker, and returns exact terminal
    /// accounting for every accepted inspection.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError`] if either background worker
    /// disconnects, panics, or reports a detached observation failure.
    pub fn shutdown_with_summary(
        mut self,
    ) -> Result<DecodeInspectionSummary, DecodeInspectionError> {
        self.stop_and_join().map(|terminal| terminal.summary)
    }

    /// Drains all submitted work and returns the summary plus profiled phase
    /// aggregates.
    ///
    /// Default constructors preserve compatibility by returning disabled,
    /// empty performance aggregates. Profiled constructors are the only paths
    /// that perform per-job monotonic clock reads.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError`] if either background worker
    /// disconnects, panics, or reports a detached observation failure.
    pub fn shutdown_with_performance(
        mut self,
    ) -> Result<DecodeInspectionTerminal, DecodeInspectionError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<DecodeInspectionTerminal, DecodeInspectionError> {
        {
            let mut state = self
                .handle
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            state.stopping = true;
        }
        let mut unavailable = false;
        let mut acknowledgements = Vec::with_capacity(self.handle.senders.len());
        for sender in self.handle.senders.iter() {
            let (response_sender, response_receiver) = mpsc::sync_channel(1);
            if sender.send(Message::Shutdown(response_sender)).is_err() {
                unavailable = true;
            } else {
                acknowledgements.push(response_receiver);
            }
        }
        let mut decode = DecodePerformance {
            profiled: self.profiled,
            ..DecodePerformance::default()
        };
        for acknowledgement in acknowledgements {
            match acknowledgement.recv() {
                Ok(worker_performance) => {
                    merge_decode_performance(&mut decode, &worker_performance);
                }
                Err(_) => unavailable = true,
            }
        }
        for join_handle in self.join_handles.drain(..) {
            if join_handle.join().is_err() {
                unavailable = true;
                self.handle
                    .state
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .worker_panicked = true;
            }
        }
        let observation_result = self
            .technical_observer
            .take()
            .map(TechnicalObservationActor::shutdown_with_performance)
            .transpose()
            .map(Option::unwrap_or_default);
        let technical = observation_result?;
        let state = self
            .handle
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if state.worker_panicked {
            return Err(DecodeInspectionError::WorkerPanicked);
        }
        if unavailable {
            return Err(DecodeInspectionError::WorkerUnavailable);
        }
        Ok(DecodeInspectionTerminal {
            summary: state.summary,
            decode,
            technical,
        })
    }
}

#[allow(clippy::needless_pass_by_value)]
impl DecodeInspectionPool {
    /// Starts `worker_count` independent provider instances without a preview
    /// cache.
    ///
    /// The factory is evaluated completely before any worker thread starts.
    /// Partial provider construction therefore fails closed without leaving a
    /// reduced-capacity pool running.
    ///
    /// # Errors
    ///
    /// Returns an error for a zero worker count, provider construction or
    /// contract mismatch, or worker startup failure.
    pub fn spawn<I, F, E>(
        catalog: CatalogHandle,
        worker_count: usize,
        factory: F,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        Self::spawn_inner(
            &catalog,
            worker_count,
            factory,
            None,
            None,
            false,
            INSPECTION_QUEUE_CAPACITY,
        )
    }

    /// Starts a bounded cached pool with one freshly constructed inspector per
    /// worker.
    ///
    /// Prefer this factory form for native providers: cloning a wrapper that
    /// internally shares one session could accidentally serialize decoding.
    ///
    /// # Errors
    ///
    /// Returns an error when the cache cannot be opened or for the same
    /// provider/pool startup failures as [`Self::spawn`].
    pub fn spawn_with_cache<I, F, E>(
        catalog: CatalogHandle,
        worker_count: usize,
        factory: F,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_inner(
            &catalog,
            worker_count,
            factory,
            Some(&cache),
            None,
            false,
            INSPECTION_QUEUE_CAPACITY,
        )
    }

    /// Starts a cached pool whose embedded camera previews are published to a
    /// session-owned sink rather than persisted as cache artifacts.
    ///
    /// Generated Shadow proxies still use the supplied cache root and replace
    /// the transient visual as soon as they are ready.
    ///
    /// # Errors
    ///
    /// Returns the same cache, provider, and worker startup errors as
    /// [`Self::spawn_with_cache`].
    pub fn spawn_with_cache_and_embedded_preview_sink<I, F, E>(
        catalog: CatalogHandle,
        worker_count: usize,
        factory: F,
        cache_root: impl Into<PathBuf>,
        embedded_preview_sink: Arc<dyn EmbeddedPreviewSink>,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_inner(
            &catalog,
            worker_count,
            factory,
            Some(&cache),
            Some(embedded_preview_sink),
            false,
            INSPECTION_QUEUE_CAPACITY,
        )
    }

    /// Starts the profiled form of [`Self::spawn_with_cache`].
    ///
    /// # Errors
    ///
    /// Returns the same cache, provider, and worker startup errors as
    /// [`Self::spawn_with_cache`].
    pub fn spawn_with_cache_profiled<I, F, E>(
        catalog: CatalogHandle,
        worker_count: usize,
        factory: F,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_inner(
            &catalog,
            worker_count,
            factory,
            Some(&cache),
            None,
            true,
            INSPECTION_QUEUE_CAPACITY,
        )
    }

    /// Starts a cached pool by cloning a provider value for every worker.
    ///
    /// This is intended for inspectors whose `Clone` implementation creates
    /// independent provider state. Native-session adapters should use the
    /// factory constructor above.
    ///
    /// # Errors
    ///
    /// Returns the same cache and worker startup errors as
    /// [`Self::spawn_with_cache`].
    pub fn spawn_with_cache_from_clone<I>(
        catalog: CatalogHandle,
        worker_count: usize,
        inspector: I,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector + Clone,
    {
        Self::spawn_with_cache(
            catalog,
            worker_count,
            move |_| Ok::<I, std::convert::Infallible>(inspector.clone()),
            cache_root,
        )
    }

    fn spawn_inner<I, F, E>(
        catalog: &CatalogHandle,
        worker_count: usize,
        mut factory: F,
        cache: Option<&ContentAddressedStore>,
        embedded_preview_sink: Option<Arc<dyn EmbeddedPreviewSink>>,
        profiled: bool,
        queue_capacity: usize,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        if worker_count == 0 {
            return Err(DecodeInspectionError::InvalidWorkerCount);
        }
        let mut inspectors = Vec::with_capacity(worker_count);
        for worker_index in 0..worker_count {
            let inspector =
                factory(worker_index).map_err(|error| DecodeInspectionError::WorkerFactory {
                    worker_index,
                    message: error.to_string(),
                })?;
            inspectors.push(inspector);
        }
        let actor = DecodeInspectionActor::spawn_many_inner(
            catalog,
            inspectors,
            cache,
            embedded_preview_sink,
            profiled,
            queue_capacity,
        )?;
        Ok(Self {
            actor,
            worker_count,
        })
    }

    #[must_use]
    pub const fn worker_count(&self) -> usize {
        self.worker_count
    }

    #[must_use]
    pub fn handle(&self) -> DecodeInspectionHandle {
        self.actor.handle()
    }

    /// Drains accepted work and stops every worker.
    ///
    /// # Errors
    ///
    /// Returns an error when a worker or the technical observer disconnected
    /// or panicked.
    pub fn shutdown(self) -> Result<(), DecodeInspectionError> {
        self.actor.shutdown()
    }

    /// Drains accepted work and returns one exact pool-wide summary.
    ///
    /// # Errors
    ///
    /// Returns the same worker and observer failures as [`Self::shutdown`].
    pub fn shutdown_with_summary(self) -> Result<DecodeInspectionSummary, DecodeInspectionError> {
        self.actor.shutdown_with_summary()
    }

    /// Drains accepted work and merges all worker phase aggregates.
    ///
    /// # Errors
    ///
    /// Returns the same worker and observer failures as [`Self::shutdown`].
    pub fn shutdown_with_performance(
        self,
    ) -> Result<DecodeInspectionTerminal, DecodeInspectionError> {
        self.actor.shutdown_with_performance()
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

    /// Returns whether this worker explicitly supports a concrete directly
    /// imported source path.
    ///
    /// This is intentionally a handle property, captured when the actor is
    /// created, so a folder scan can reject unsupported files before it puts
    /// work into the bounded inspection queue. Original raster support is
    /// extension-specific: it is not enough to know that a source is merely
    /// `OriginalRaster`.
    pub fn supports_source(&self, kind: RepresentationKind, path: &Path) -> bool {
        match kind {
            RepresentationKind::OriginalRaw => self.supports_original_raw,
            RepresentationKind::OriginalRaster => path
                .extension()
                .and_then(|extension| extension.to_str())
                .is_some_and(|extension| {
                    self.supported_original_raster_extensions
                        .iter()
                        .any(|supported| supported.eq_ignore_ascii_case(extension))
                }),
            RepresentationKind::DerivedDng
            | RepresentationKind::EmbeddedPreview
            | RepresentationKind::SceneLinearRgb
            | RepresentationKind::VendorRenderedRgb
            | RepresentationKind::Proxy => false,
        }
    }

    pub const fn caches_previews(&self) -> bool {
        self.caches_previews
    }

    pub fn technical_preprocessing_version(&self) -> Option<&str> {
        self.technical_preprocessing_version.as_deref()
    }

    /// Returns a cheap, non-blocking snapshot suitable for import progress.
    ///
    /// This intentionally reports visual publications separately from completed
    /// inspections: a Library can show an embedded camera preview while the
    /// same worker continues creating its deterministic generated proxy.
    #[must_use]
    pub fn progress_snapshot(&self) -> DecodeInspectionProgress {
        let state = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        DecodeInspectionProgress {
            summary: state.summary,
            visual_artifacts_published: state.visual_artifacts_published,
        }
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
        self.submit_with_cancellation(request, &ScanCancellation::new())
    }

    /// Queues an inspection governed by the same token as its parent scan.
    ///
    /// Waiting for space in the bounded queue remains cancellation-responsive.
    /// A job cancelled before provider work returns a discarded outcome rather
    /// than an error.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::WorkerUnavailable`] after shutdown or
    /// worker disconnection.
    pub fn submit_with_cancellation(
        &self,
        request: DecodeInspectionRequest,
        cancellation: &ScanCancellation,
    ) -> Result<DecodeInspectionTicket, DecodeInspectionError> {
        self.submit_with_cancellation_inner(request, cancellation, false, || {})
            .map(|submission| submission.ticket)
    }

    pub(crate) fn submit_with_cancellation_observed(
        &self,
        request: DecodeInspectionRequest,
        cancellation: &ScanCancellation,
    ) -> Result<DecodeInspectionSubmission, DecodeInspectionError> {
        self.submit_with_cancellation_inner(request, cancellation, true, || {})
    }

    fn submit_with_cancellation_inner(
        &self,
        request: DecodeInspectionRequest,
        cancellation: &ScanCancellation,
        observe_queue_full: bool,
        mut queue_full_hook: impl FnMut(),
    ) -> Result<DecodeInspectionSubmission, DecodeInspectionError> {
        let (response_sender, response_receiver) = mpsc::channel();
        let mut message = Some(Message::Inspect(InspectionMessage {
            request,
            cancellation: cancellation.clone(),
            response: response_sender,
            enqueued_at: self.profiled.then(Instant::now),
        }));
        let mut queue_full_events = 0_u64;
        let worker_count = self.senders.len();
        let mut first_worker = self.next_worker.fetch_add(1, Ordering::Relaxed) % worker_count;
        loop {
            let mut state = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if state.stopping {
                return Err(DecodeInspectionError::WorkerUnavailable);
            }
            if cancellation.is_cancelled() {
                if let Some(Message::Inspect(InspectionMessage { response, .. })) = message.take() {
                    let result = Ok(DecodeInspectionOutcome::Discarded(
                        DecodeInspectionDiscardReason::Cancelled,
                    ));
                    state.summary.record(&result);
                    let _ = response.send(result);
                }
                break;
            }
            let mut sent = false;
            let mut disconnected = false;
            for offset in 0..worker_count {
                let worker_index = (first_worker + offset) % worker_count;
                let pending = message
                    .take()
                    .expect("inspection message remains owned until submitted");
                match self.senders[worker_index].try_send(pending) {
                    Ok(()) => {
                        self.next_worker
                            .store(worker_index.wrapping_add(1), Ordering::Relaxed);
                        sent = true;
                        break;
                    }
                    Err(mpsc::TrySendError::Full(returned)) => {
                        message = Some(returned);
                    }
                    Err(mpsc::TrySendError::Disconnected(returned)) => {
                        message = Some(returned);
                        state.stopping = true;
                        disconnected = true;
                        break;
                    }
                }
            }
            drop(state);
            if sent {
                break;
            }
            if disconnected {
                return Err(DecodeInspectionError::WorkerUnavailable);
            }
            if observe_queue_full {
                queue_full_events = queue_full_events.saturating_add(1);
            }
            queue_full_hook();
            thread::park_timeout(Duration::from_millis(1));
            if let Some(Message::Inspect(inspection)) = &mut message
                && inspection.enqueued_at.is_some()
            {
                inspection.enqueued_at = Some(Instant::now());
            }
            first_worker = first_worker.wrapping_add(1) % worker_count;
        }
        Ok(DecodeInspectionSubmission {
            ticket: DecodeInspectionTicket {
                receiver: response_receiver,
            },
            queue_full_events,
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
    embedded_preview_sink: Option<&dyn EmbeddedPreviewSink>,
    receiver: &Receiver<Message>,
    state: &Mutex<DecodeInspectionState>,
    profiled: bool,
) {
    let mut performance = DecodePerformance {
        profiled,
        ..DecodePerformance::default()
    };
    while let Ok(message) = receiver.recv() {
        match message {
            Message::Inspect(inspection) => {
                if let Some(enqueued_at) = inspection.enqueued_at {
                    performance.queue_wait.record(enqueued_at.elapsed());
                }
                let result = if inspection.cancellation.is_cancelled() {
                    Ok(DecodeInspectionOutcome::Discarded(
                        DecodeInspectionDiscardReason::Cancelled,
                    ))
                } else {
                    inspect_and_record(
                        catalog,
                        &mut inspector,
                        cache,
                        technical_observer,
                        embedded_preview_sink,
                        &inspection.request,
                        &inspection.cancellation,
                        state,
                        &mut performance,
                    )
                };
                state
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .summary
                    .record(&result);
                let _ = inspection.response.send(result);
            }
            Message::Shutdown(response) => {
                let _ = response.send(performance);
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
    embedded_preview_sink: Option<&dyn EmbeddedPreviewSink>,
    request: &DecodeInspectionRequest,
    cancellation: &ScanCancellation,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }
    if read_source_fingerprint_profiled(&request.path, performance)? != request.expected_source {
        return Ok(DecodeInspectionOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }

    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }
    let snapshot = measure_if(
        performance.profiled,
        &mut performance.provider_inspect,
        || inspector.inspect(&request.path),
    )
    .map_err(|message| DecodeInspectionError::Inspector {
        path: request.path.clone(),
        message,
    })?;
    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }
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

    if read_source_fingerprint_profiled(&request.path, performance)? != request.expected_source {
        return Ok(DecodeInspectionOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }
    if cancellation.is_cancelled() {
        return Ok(cancelled_outcome());
    }

    let provider_id = snapshot.provider.id.clone();
    let provider_version = snapshot.provider.version.clone();
    let status = measure_if(
        performance.profiled,
        &mut performance.snapshot_catalog_commit,
        || {
            catalog.record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id: request.representation_id,
                expected_source: request.expected_source,
                snapshot,
                inspected_at_ms: now_ms(),
            })
        },
    )?;

    Ok(match status {
        RecordDecodeSnapshotStatus::Recorded => {
            let preview = if cancellation.is_cancelled() {
                PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
            } else {
                cache.map_or(PreviewCacheOutcome::NotRequested, |cache| {
                    cache_preview_with_context(
                        inspector,
                        PreviewCacheContext {
                            catalog,
                            cache,
                            technical_observer,
                            embedded_preview_sink,
                            request,
                            provider_id: &provider_id,
                            provider_version: &provider_version,
                            cancellation,
                        },
                        inspection_state,
                        performance,
                    )
                })
            };
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

#[derive(Copy, Clone)]
struct PreviewCacheContext<'a> {
    catalog: &'a CatalogHandle,
    cache: &'a ContentAddressedStore,
    technical_observer: Option<&'a TechnicalObservationHandle>,
    embedded_preview_sink: Option<&'a dyn EmbeddedPreviewSink>,
    request: &'a DecodeInspectionRequest,
    provider_id: &'a str,
    provider_version: &'a str,
    cancellation: &'a ScanCancellation,
}

fn cache_preview_with_context(
    inspector: &mut impl DecodeInspector,
    context: PreviewCacheContext<'_>,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    // A camera preview is the fast path to a populated Library, not the final
    // Shadow rendering contract. Desktop sessions may keep it only in memory;
    // callers without a sink retain the legacy durable-cache behavior. In both
    // cases a generated proxy continues from the same source and replaces the
    // temporary visual when its deterministic render is recorded.
    let mut fallback = None;
    match extract_embedded_preview(inspector, context, performance) {
        Ok(Some(preview)) => {
            let outcome = if let Some(sink) = context.embedded_preview_sink {
                publish_embedded_preview(context, sink, preview, inspection_state, performance)
            } else {
                let prepared = prepare_embedded_cached_visual(context, preview);
                store_prepared_cached_visual(context, prepared, inspection_state, performance)
            };
            if matches!(outcome, PreviewCacheOutcome::Discarded(_)) {
                return outcome;
            }
            fallback = Some(outcome);
        }
        Ok(None) => {}
        Err(outcome) => {
            if matches!(outcome, PreviewCacheOutcome::Discarded(_)) {
                return outcome;
            }
            fallback = Some(outcome);
        }
    }

    match prepare_generated_proxy(inspector, context, performance) {
        Ok(Some(prepared)) => {
            let outcome =
                store_prepared_cached_visual(context, prepared, inspection_state, performance);
            match outcome {
                PreviewCacheOutcome::StoredGeneratedProxy { .. }
                | PreviewCacheOutcome::Discarded(_) => outcome,
                failed => fallback.unwrap_or(failed),
            }
        }
        Ok(None) => fallback.unwrap_or(PreviewCacheOutcome::NoVisualAvailable),
        Err(outcome) => {
            if matches!(outcome, PreviewCacheOutcome::Discarded(_)) {
                outcome
            } else {
                fallback.unwrap_or(outcome)
            }
        }
    }
}

type PreparedCachedVisual = (Vec<u8>, CachedArtifact, CachedVisualKind);

fn store_prepared_cached_visual(
    context: PreviewCacheContext<'_>,
    prepared: PreparedCachedVisual,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    let (bytes, artifact, stored_kind) = prepared;
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    let blob = match measure_if(
        performance.profiled,
        &mut performance.cache_blob_put,
        || context.cache.put(&bytes),
    ) {
        Ok(blob) => blob,
        Err(error) => return PreviewCacheOutcome::Failed(error.to_string()),
    };
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    let artifact = CachedArtifact {
        blob_algorithm: blob.digest.algorithm().to_owned(),
        blob_digest: *blob.digest.as_bytes(),
        blob_byte_len: blob.byte_len,
        created_at_ms: now_ms(),
        ..artifact
    };
    let outcome = record_cached_visual(
        context.catalog,
        context.technical_observer,
        context.request,
        artifact,
        stored_kind,
        &blob,
        performance,
    );
    count_published_visual(outcome.clone(), inspection_state);
    outcome
}

fn count_published_visual(
    outcome: PreviewCacheOutcome,
    inspection_state: &Mutex<DecodeInspectionState>,
) {
    if matches!(
        outcome,
        PreviewCacheOutcome::PublishedEmbeddedPreview { .. }
            | PreviewCacheOutcome::StoredEmbeddedPreview { .. }
            | PreviewCacheOutcome::StoredGeneratedProxy { .. }
    ) {
        let mut state = inspection_state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        state.visual_artifacts_published = state.visual_artifacts_published.saturating_add(1);
    }
}

fn publish_embedded_preview(
    context: PreviewCacheContext<'_>,
    sink: &dyn EmbeddedPreviewSink,
    preview: PreviewPayload,
    inspection_state: &Mutex<DecodeInspectionState>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    let byte_len = u64::try_from(preview.bytes.len()).unwrap_or(u64::MAX);
    let outcome = match sink.publish_embedded_preview(EmbeddedPreviewPublication {
        representation_id: context.request.representation_id,
        source: context.request.expected_source,
        preview,
    }) {
        Ok(()) => PreviewCacheOutcome::PublishedEmbeddedPreview { byte_len },
        Err(error) => PreviewCacheOutcome::Failed(error),
    };
    count_published_visual(outcome.clone(), inspection_state);
    outcome
}

fn extract_embedded_preview(
    inspector: &mut impl DecodeInspector,
    context: PreviewCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<Option<PreviewPayload>, PreviewCacheOutcome> {
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let preview = measure_if(
        performance.profiled,
        &mut performance.embedded_preview_extract,
        || inspector.extract_best_preview(&context.request.path),
    )
    .map_err(PreviewCacheOutcome::Failed)?;
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    if source_changed_profiled(context.request, performance) {
        return Err(PreviewCacheOutcome::Discarded(
            DecodeInspectionDiscardReason::FilesystemChanged,
        ));
    }
    Ok(preview)
}

fn prepare_embedded_cached_visual(
    context: PreviewCacheContext<'_>,
    preview: PreviewPayload,
) -> PreparedCachedVisual {
    let artifact = CachedArtifact {
        role: CachedArtifactRole::EmbeddedPreview,
        variant_key: context.provider_id.to_owned(),
        generator_id: context.provider_id.to_owned(),
        generator_version: context.provider_version.to_owned(),
        recipe_snapshot_digest: None,
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
}

fn prepare_generated_proxy(
    inspector: &mut impl DecodeInspector,
    context: PreviewCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<Option<PreparedCachedVisual>, PreviewCacheOutcome> {
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let proxy = measure_if(performance.profiled, &mut performance.proxy_render, || {
        inspector.render_proxy(&context.request.path)
    })
    .map_err(PreviewCacheOutcome::Failed)?;
    let Some(proxy) = proxy else {
        return Ok(None);
    };
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let dimensions = proxy.dimensions;
    let artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: inspector.proxy_variant_key().to_owned(),
        generator_id: context.provider_id.to_owned(),
        generator_version: context.provider_version.to_owned(),
        recipe_snapshot_digest: None,
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
    Ok(Some((
        proxy.bytes,
        artifact,
        CachedVisualKind::GeneratedProxy(dimensions),
    )))
}

fn cancelled_outcome() -> DecodeInspectionOutcome {
    DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
}

fn cancelled_preview() -> PreviewCacheOutcome {
    PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
}

fn record_cached_visual(
    catalog: &CatalogHandle,
    technical_observer: Option<&TechnicalObservationHandle>,
    request: &DecodeInspectionRequest,
    artifact: CachedArtifact,
    stored_kind: CachedVisualKind,
    blob: &StoredBlob,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    let status = measure_if(
        performance.profiled,
        &mut performance.cache_artifact_catalog_commit,
        || {
            catalog.record_cached_artifact(&RecordCachedArtifact {
                representation_id: request.representation_id,
                expected_source: request.expected_source,
                artifact,
            })
        },
    );
    match status {
        Ok(RecordCachedArtifactStatus::Recorded) => {
            if let Some(observer) = technical_observer {
                let _ = measure_if(
                    performance.profiled,
                    &mut performance.technical_submit_wait,
                    || {
                        observer.submit_preferred_detached(
                            request.representation_id,
                            request.expected_source,
                        )
                    },
                );
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

fn source_changed_profiled(
    request: &DecodeInspectionRequest,
    performance: &mut DecodePerformance,
) -> bool {
    measure_if(
        performance.profiled,
        &mut performance.source_guard_stat,
        || fingerprint_source(&request.path),
    )
    .map_or(true, |current| current != request.expected_source)
}

fn read_source_fingerprint_profiled(
    path: &Path,
    performance: &mut DecodePerformance,
) -> Result<RepresentationFingerprint, DecodeInspectionError> {
    measure_if(
        performance.profiled,
        &mut performance.source_guard_stat,
        || fingerprint_source(path),
    )
    .map_err(|source| DecodeInspectionError::SourceMetadata {
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
    use std::{
        fs,
        sync::{
            Arc, Mutex,
            atomic::{AtomicUsize, Ordering},
            mpsc,
        },
    };

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

        let terminal = worker
            .shutdown_with_performance()
            .expect("shutdown default inspector with terminal profile");
        assert_eq!(
            terminal.summary,
            DecodeInspectionSummary {
                completed: 2,
                hard_failures: 0,
                preview_failures: 0,
                cancelled: 0,
            }
        );
        assert!(!terminal.decode.profiled);
        assert_eq!(terminal.decode, DecodePerformance::default());
        assert_eq!(terminal.technical, TechnicalPerformance::default());
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn pool_runs_independent_inspectors_concurrently_and_merges_performance() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let (entered_sender, entered_receiver) = mpsc::channel();
        let (release_zero_sender, release_zero_receiver) = mpsc::sync_channel(0);
        let (release_one_sender, release_one_receiver) = mpsc::sync_channel(0);
        let mut releases = [Some(release_zero_receiver), Some(release_one_receiver)];
        let active = Arc::new(AtomicUsize::new(0));
        let maximum_active = Arc::new(AtomicUsize::new(0));
        let pool = DecodeInspectionPool::spawn_inner(
            &catalog,
            2,
            |worker_index| {
                let entered_sender = entered_sender.clone();
                let release = releases[worker_index]
                    .take()
                    .expect("one release gate per inspector");
                let active = Arc::clone(&active);
                let maximum_active = Arc::clone(&maximum_active);
                Ok::<_, String>(move |_path: &Path| {
                    let now_active = active.fetch_add(1, Ordering::SeqCst) + 1;
                    maximum_active.fetch_max(now_active, Ordering::SeqCst);
                    entered_sender
                        .send(worker_index)
                        .expect("report pool worker entry");
                    release.recv().expect("release pool worker");
                    active.fetch_sub(1, Ordering::SeqCst);
                    Ok(sample_snapshot())
                })
            },
            None,
            None,
            true,
            2,
        )
        .expect("spawn two-worker pool");
        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };
        let handle = pool.handle();
        let first = handle
            .submit(request.clone())
            .expect("submit first pool job");
        let second = handle.submit(request).expect("submit second pool job");

        let first_worker = entered_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("first worker enters");
        let second_worker = entered_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("second worker enters concurrently");
        assert_ne!(first_worker, second_worker);
        release_zero_sender.send(()).expect("release worker zero");
        release_one_sender.send(()).expect("release worker one");
        first.wait().expect("first pool job completes");
        second.wait().expect("second pool job completes");

        let terminal = pool
            .shutdown_with_performance()
            .expect("drain pool with aggregate performance");
        assert_eq!(terminal.summary.completed, 2);
        assert_eq!(terminal.decode.provider_inspect.samples, 2);
        assert_eq!(maximum_active.load(Ordering::SeqCst), 2);
        assert_eq!(
            catalog
                .decode_snapshots(registered.representation_id)
                .expect("read pool-written snapshot")
                .len(),
            1
        );
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn pool_factory_failure_starts_no_reduced_capacity_worker_set() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let provider_calls = Arc::new(AtomicUsize::new(0));
        let factory_calls = Arc::new(AtomicUsize::new(0));
        let result = DecodeInspectionPool::spawn(actor.handle(), 2, {
            let provider_calls = Arc::clone(&provider_calls);
            let factory_calls = Arc::clone(&factory_calls);
            move |worker_index| {
                factory_calls.fetch_add(1, Ordering::SeqCst);
                if worker_index == 1 {
                    return Err("fixture factory failure");
                }
                let provider_calls = Arc::clone(&provider_calls);
                Ok(move |_path: &Path| {
                    provider_calls.fetch_add(1, Ordering::SeqCst);
                    Ok(sample_snapshot())
                })
            }
        });
        assert!(matches!(
            result,
            Err(DecodeInspectionError::WorkerFactory {
                worker_index: 1,
                ..
            })
        ));
        assert_eq!(factory_calls.load(Ordering::SeqCst), 2);
        assert_eq!(provider_calls.load(Ordering::SeqCst), 0);
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn pool_provider_panic_closes_the_whole_submission_gate() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let pool = DecodeInspectionPool::spawn(catalog, 2, |worker_index| {
            Ok::<_, String>(move |_path: &Path| {
                assert_ne!(worker_index, 0, "intentional pool provider panic");
                Ok(sample_snapshot())
            })
        })
        .expect("spawn panic fixture pool");
        let handle = pool.handle();
        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };
        assert!(matches!(
            handle
                .submit(request.clone())
                .expect("submit panic fixture")
                .wait(),
            Err(DecodeInspectionError::WorkerUnavailable)
        ));
        let deadline = Instant::now() + Duration::from_secs(1);
        loop {
            if matches!(
                handle.submit(request.clone()),
                Err(DecodeInspectionError::WorkerUnavailable)
            ) {
                break;
            }
            assert!(
                Instant::now() < deadline,
                "pool did not close after provider panic"
            );
            thread::yield_now();
        }
        assert!(matches!(
            pool.shutdown_with_summary(),
            Err(DecodeInspectionError::WorkerPanicked)
        ));
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn recommended_pool_size_is_explicitly_memory_bounded() {
        assert!(
            (MIN_RECOMMENDED_INSPECTION_WORKERS..=MAX_RECOMMENDED_INSPECTION_WORKERS)
                .contains(&recommended_decode_inspection_worker_count())
        );
    }

    #[test]
    fn shutdown_summary_accounts_for_success_failures_and_local_cancellation() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let worker = DecodeInspectionActor::spawn_with_cache(
            catalog.clone(),
            SummaryInspector::default(),
            fixture.root.join("cache"),
        )
        .expect("spawn summary inspector");
        let handle = worker.handle();
        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };

        let success = handle
            .submit(request.clone())
            .expect("submit successful inspection");
        let hard_failure = handle
            .submit(request.clone())
            .expect("submit hard-failing inspection");
        let preview_failure = handle
            .submit(request.clone())
            .expect("submit preview-failing inspection");
        let cancelled_token = ScanCancellation::new();
        cancelled_token.cancel();
        let cancelled = handle
            .submit_with_cancellation(request, &cancelled_token)
            .expect("accept locally cancelled inspection");

        assert!(matches!(
            success.wait().expect("complete successful inspection"),
            DecodeInspectionOutcome::Recorded {
                preview: PreviewCacheOutcome::NoVisualAvailable,
                ..
            }
        ));
        assert!(matches!(
            hard_failure.wait(),
            Err(DecodeInspectionError::Inspector { .. })
        ));
        assert!(matches!(
            preview_failure
                .wait()
                .expect("complete preview-failing inspection"),
            DecodeInspectionOutcome::Recorded {
                preview: PreviewCacheOutcome::Failed(_),
                ..
            }
        ));
        assert_eq!(
            cancelled.wait().expect("complete cancelled inspection"),
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
        );

        assert_eq!(
            worker
                .shutdown_with_summary()
                .expect("shutdown inspector with summary"),
            DecodeInspectionSummary {
                completed: 4,
                hard_failures: 1,
                preview_failures: 1,
                cancelled: 1,
            }
        );
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn submission_gate_rejects_a_concurrent_submit_at_the_shutdown_boundary() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let calls = Arc::new(AtomicUsize::new(0));
        let inspector_calls = Arc::clone(&calls);
        let worker = DecodeInspectionActor::spawn(catalog, move |_path: &Path| {
            inspector_calls.fetch_add(1, Ordering::SeqCst);
            Ok(sample_snapshot())
        })
        .expect("spawn inspector");
        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };
        let state = Arc::clone(&worker.handle.state);
        let mut closing_gate = state.lock().expect("hold submission gate");
        let handle = worker.handle();
        let (started_sender, started_receiver) = mpsc::sync_channel(0);
        let submit_thread = thread::spawn(move || {
            started_sender.send(()).expect("announce concurrent submit");
            handle.submit(request)
        });
        started_receiver
            .recv()
            .expect("concurrent submit reaches gate");

        closing_gate.stopping = true;
        drop(closing_gate);
        assert!(matches!(
            submit_thread.join().expect("join concurrent submit"),
            Err(DecodeInspectionError::WorkerUnavailable)
        ));
        assert_eq!(calls.load(Ordering::SeqCst), 0);
        assert_eq!(
            worker
                .shutdown_with_summary()
                .expect("shutdown closed inspector"),
            DecodeInspectionSummary::default()
        );
        actor.shutdown().expect("shutdown catalog");
    }

    #[test]
    fn profiled_submission_counts_full_queue_retries_without_timing_thresholds() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
        let (release_sender, release_receiver) = mpsc::sync_channel(0);
        let mut first_job = true;
        let worker = DecodeInspectionActor::spawn_inner_with_capacity(
            &catalog,
            move |_path: &Path| {
                if first_job {
                    first_job = false;
                    entered_sender.send(()).expect("announce first job");
                    release_receiver.recv().expect("release first job");
                }
                Ok(sample_snapshot())
            },
            None,
            true,
            1,
        )
        .expect("spawn one-slot profiled worker");
        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };
        let handle = worker.handle();
        let first = handle.submit(request.clone()).expect("submit active job");
        entered_receiver.recv().expect("first job enters provider");
        let second = handle
            .submit(request.clone())
            .expect("fill the bounded queue");

        let third_handle = handle.clone();
        let (full_sender, full_receiver) = mpsc::sync_channel(0);
        let third = thread::spawn(move || {
            let mut announced = false;
            third_handle.submit_with_cancellation_inner(
                request,
                &ScanCancellation::new(),
                true,
                || {
                    if !announced {
                        full_sender.send(()).expect("announce full queue");
                        announced = true;
                    }
                },
            )
        });
        full_receiver
            .recv()
            .expect("third submit deterministically observes a full queue");
        release_sender.send(()).expect("release active job");
        let third = third
            .join()
            .expect("join third submit")
            .expect("submit after queue frees");

        assert!(third.queue_full_events >= 1);
        first.wait().expect("finish first job");
        second.wait().expect("finish second job");
        third.ticket.wait().expect("finish third job");
        let terminal = worker
            .shutdown_with_performance()
            .expect("shutdown profiled worker");
        assert_eq!(terminal.summary.completed, 3);
        assert_eq!(terminal.decode.queue_wait.samples, 3);

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
    fn queued_cancelled_inspections_never_begin_provider_work() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
        let registered = catalog
            .register_asset(&fixture.registration(source))
            .expect("register source");
        let calls = Arc::new(AtomicUsize::new(0));
        let inspector_calls = Arc::clone(&calls);
        let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
        let (release_sender, release_receiver) = mpsc::sync_channel(0);
        let worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
            inspector_calls.fetch_add(1, Ordering::SeqCst);
            entered_sender.send(()).expect("announce provider entry");
            release_receiver.recv().expect("release provider");
            Ok(sample_snapshot())
        })
        .expect("spawn inspector");
        let request = DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        };
        let cancellation = ScanCancellation::new();
        let first = worker
            .handle()
            .submit_with_cancellation(request.clone(), &cancellation)
            .expect("submit active inspection");
        entered_receiver.recv().expect("provider starts first job");
        let queued = (0..3)
            .map(|_| {
                worker
                    .handle()
                    .submit_with_cancellation(request.clone(), &cancellation)
                    .expect("submit queued inspection")
            })
            .collect::<Vec<_>>();

        cancellation.cancel();
        release_sender.send(()).expect("release active inspection");
        assert_eq!(
            first.wait().expect("active job observes cancellation"),
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
        );
        for ticket in queued {
            assert_eq!(
                ticket.wait().expect("queued job is discarded"),
                DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
            );
        }
        assert_eq!(calls.load(Ordering::SeqCst), 1);
        assert!(
            catalog
                .decode_snapshots(registered.representation_id)
                .expect("read snapshots")
                .is_empty()
        );

        assert_eq!(
            worker
                .shutdown_with_summary()
                .expect("shutdown inspector with cancellation summary"),
            DecodeInspectionSummary {
                completed: 4,
                hard_failures: 0,
                preview_failures: 0,
                cancelled: 4,
            }
        );
        actor.shutdown().expect("shutdown catalog");
    }

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
            } if byte_len == u64::try_from(TEST_DISPLAY_JPEG.len()).expect("test JPEG length fits")
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
                TEST_DISPLAY_JPEG
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

    #[derive(Debug, Default)]
    struct SummaryInspector {
        inspect_calls: usize,
        preview_calls: usize,
    }

    impl DecodeInspector for SummaryInspector {
        fn inspect(&mut self, _path: &Path) -> Result<DecoderSnapshot, String> {
            self.inspect_calls += 1;
            if self.inspect_calls == 2 {
                Err("hard inspection fixture".into())
            } else {
                Ok(sample_snapshot())
            }
        }

        fn extract_best_preview(&mut self, _path: &Path) -> Result<Option<PreviewPayload>, String> {
            self.preview_calls += 1;
            if self.preview_calls == 2 {
                Err("preview failure fixture".into())
            } else {
                Ok(None)
            }
        }
    }

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
                bytes: TEST_DISPLAY_JPEG.to_vec(),
            }))
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
                bytes: TEST_DISPLAY_JPEG.to_vec(),
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
                iso_speed: 0.0,
                exposure_time_seconds: 0.0,
                aperture_f_number: 0.0,
                focal_length_mm: 0.0,
                captured_at_unix_seconds: 0,
                lens_make: String::new(),
                lens_model: String::new(),
                focal_length_35mm: 0.0,
            },
            capabilities: DecodeCapabilitySnapshot {
                metadata: DecodeSupport::Available,
                embedded_previews: DecodeSupport::Unavailable,
                raw_frame: DecodeSupport::Available,
                reference_rgb: DecodeSupport::Unavailable,
                pending_corrections: PendingCorrectionsSnapshot::default(),
                raw_development: Default::default(),
            },
            previews: Vec::new(),
        }
    }
}

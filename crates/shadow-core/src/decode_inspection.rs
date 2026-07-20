use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
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
    RepresentationId,
};
use thiserror::Error;

use crate::import::ScanCancellation;
use crate::performance::{DecodePerformance, TechnicalPerformance, measure_if};
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
    profiled: bool,
}

#[derive(Debug, Clone)]
pub struct DecodeInspectionHandle {
    sender: SyncSender<Message>,
    state: Arc<Mutex<DecodeInspectionState>>,
    provider_id: Arc<str>,
    provider_version: Arc<str>,
    proxy_variant_key: Arc<str>,
    technical_preprocessing_version: Option<Arc<str>>,
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
    summary: DecodeInspectionSummary,
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

pub(crate) struct DecodeInspectionSubmission {
    pub ticket: DecodeInspectionTicket,
    pub queue_full_events: u64,
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
        Self::spawn_inner(catalog, inspector, None, false)
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
        Self::spawn_inner(catalog, inspector, None, true)
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
        Self::spawn_inner(catalog, inspector, Some(cache), false)
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
        Self::spawn_inner(catalog, inspector, Some(cache), true)
    }

    fn spawn_inner(
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
        cache: Option<ContentAddressedStore>,
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
        catalog: CatalogHandle,
        inspector: impl DecodeInspector,
        cache: Option<ContentAddressedStore>,
        profiled: bool,
        queue_capacity: usize,
    ) -> Result<Self, DecodeInspectionError> {
        let provider_id = Arc::<str>::from(inspector.provider_id());
        let provider_version = Arc::<str>::from(inspector.provider_version());
        let proxy_variant_key = Arc::<str>::from(inspector.proxy_variant_key());
        let caches_previews = cache.is_some();
        let technical_observer = cache
            .as_ref()
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
        let (sender, receiver) = mpsc::sync_channel(queue_capacity);
        let state = Arc::new(Mutex::new(DecodeInspectionState::default()));
        let worker_state = Arc::clone(&state);
        let join_handle = thread::Builder::new()
            .name("shadow-decode-inspector".to_owned())
            .spawn(move || {
                run_worker(
                    &catalog,
                    inspector,
                    cache.as_ref(),
                    technical_handle.as_ref(),
                    &receiver,
                    &worker_state,
                    profiled,
                );
            })
            .map_err(DecodeInspectionError::WorkerStart)?;
        Ok(Self {
            handle: DecodeInspectionHandle {
                sender,
                state,
                provider_id,
                provider_version,
                proxy_variant_key,
                technical_preprocessing_version,
                caches_previews,
                profiled,
            },
            join_handle: Some(join_handle),
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
        let decode_result = self.join_handle.take().map_or_else(
            || {
                Ok(DecodePerformance {
                    profiled: self.profiled,
                    ..DecodePerformance::default()
                })
            },
            |join_handle| {
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
            },
        );
        let observation_result = self
            .technical_observer
            .take()
            .map(TechnicalObservationActor::shutdown_with_performance)
            .transpose()
            .map(Option::unwrap_or_default);
        let decode = decode_result?;
        let technical = observation_result?;
        let state = self
            .handle
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        Ok(DecodeInspectionTerminal {
            summary: state.summary,
            decode,
            technical,
        })
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
        let mut message = Message::Inspect(InspectionMessage {
            request,
            cancellation: cancellation.clone(),
            response: response_sender,
            enqueued_at: self.profiled.then(Instant::now),
        });
        let mut queue_full_events = 0_u64;
        loop {
            let mut state = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if state.stopping {
                return Err(DecodeInspectionError::WorkerUnavailable);
            }
            if cancellation.is_cancelled() {
                if let Message::Inspect(InspectionMessage { response, .. }) = message {
                    let result = Ok(DecodeInspectionOutcome::Discarded(
                        DecodeInspectionDiscardReason::Cancelled,
                    ));
                    state.summary.record(&result);
                    let _ = response.send(result);
                }
                break;
            }
            let send_result = self.sender.try_send(message);
            drop(state);
            match send_result {
                Ok(()) => break,
                Err(mpsc::TrySendError::Full(mut returned)) => {
                    if observe_queue_full {
                        queue_full_events = queue_full_events.saturating_add(1);
                    }
                    queue_full_hook();
                    thread::park_timeout(Duration::from_millis(1));
                    if let Message::Inspect(inspection) = &mut returned
                        && inspection.enqueued_at.is_some()
                    {
                        inspection.enqueued_at = Some(Instant::now());
                    }
                    message = returned;
                }
                Err(mpsc::TrySendError::Disconnected(_)) => {
                    return Err(DecodeInspectionError::WorkerUnavailable);
                }
            }
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
                        &inspection.request,
                        &inspection.cancellation,
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
    request: &DecodeInspectionRequest,
    cancellation: &ScanCancellation,
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
                            request,
                            provider_id: &provider_id,
                            provider_version: &provider_version,
                            cancellation,
                        },
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

#[derive(Debug, Copy, Clone)]
struct PreviewCacheContext<'a> {
    catalog: &'a CatalogHandle,
    cache: &'a ContentAddressedStore,
    technical_observer: Option<&'a TechnicalObservationHandle>,
    request: &'a DecodeInspectionRequest,
    provider_id: &'a str,
    provider_version: &'a str,
    cancellation: &'a ScanCancellation,
}

fn cache_preview_with_context(
    inspector: &mut impl DecodeInspector,
    context: PreviewCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> PreviewCacheOutcome {
    if context.cancellation.is_cancelled() {
        return cancelled_preview();
    }
    if source_changed_profiled(context.request, performance) {
        return PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged);
    }
    let (bytes, artifact, stored_kind) =
        match prepare_cached_visual(inspector, context, performance) {
            Ok(prepared) => prepared,
            Err(outcome) => return outcome,
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
    record_cached_visual(
        context.catalog,
        context.technical_observer,
        context.request,
        artifact,
        stored_kind,
        &blob,
        performance,
    )
}

fn prepare_cached_visual(
    inspector: &mut impl DecodeInspector,
    context: PreviewCacheContext<'_>,
    performance: &mut DecodePerformance,
) -> Result<(Vec<u8>, CachedArtifact, CachedVisualKind), PreviewCacheOutcome> {
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
    if let Some(preview) = preview {
        let artifact = CachedArtifact {
            role: CachedArtifactRole::EmbeddedPreview,
            variant_key: context.provider_id.to_owned(),
            generator_id: context.provider_id.to_owned(),
            generator_version: context.provider_version.to_owned(),
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
        return Ok((preview.bytes, artifact, CachedVisualKind::EmbeddedPreview));
    }
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let proxy = measure_if(performance.profiled, &mut performance.proxy_render, || {
        inspector.render_proxy(&context.request.path)
    })
    .map_err(PreviewCacheOutcome::Failed)?
    .ok_or(PreviewCacheOutcome::NoVisualAvailable)?;
    if context.cancellation.is_cancelled() {
        return Err(cancelled_preview());
    }
    let dimensions = proxy.dimensions;
    let artifact = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: inspector.proxy_variant_key().to_owned(),
        generator_id: context.provider_id.to_owned(),
        generator_version: context.provider_version.to_owned(),
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
    Ok((
        proxy.bytes,
        artifact,
        CachedVisualKind::GeneratedProxy(dimensions),
    ))
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
            Arc,
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
            catalog,
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
    fn embedded_preview_is_content_addressed_and_cataloged() {
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

        let terminal = worker
            .shutdown_with_performance()
            .expect("shutdown profiled inspector");
        assert!(terminal.decode.profiled);
        assert_eq!(terminal.decode.provider_inspect.samples, 1);
        assert_eq!(terminal.decode.embedded_preview_extract.samples, 1);
        assert_eq!(terminal.decode.proxy_render.samples, 0);
        assert_eq!(terminal.decode.cache_blob_put.samples, 1);
        assert_eq!(terminal.decode.cache_artifact_catalog_commit.samples, 1);
        assert_eq!(terminal.decode.technical_submit_wait.samples, 1);
        assert!(terminal.technical.profiled);
        assert_eq!(terminal.technical.technical_observation_total.samples, 1);
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

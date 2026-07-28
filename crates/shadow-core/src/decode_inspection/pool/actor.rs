//! Decode worker-group construction, lifecycle, shutdown, and performance merging.

use std::{
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::AtomicUsize,
        mpsc::{self, SyncSender},
    },
    thread::{self, JoinHandle},
};

use shadow_cache::ContentAddressedStore;
use shadow_catalog::CatalogHandle;

use crate::performance::{DecodePerformance, DurationStats};
use crate::technical_observation::{
    TechnicalObservationActor, TechnicalObservationHandle, technical_analysis_preprocessing_version,
};

use super::super::contract::{
    DecodeInspectionError, DecodeInspectionSummary, DecodeInspectionTerminal, DecodeInspector,
    EmbeddedPreviewSink,
};
use super::super::runtime_state::DecodeInspectionState;
use super::super::submission::{DecodeInspectionHandle, Message};
use super::super::worker::{WorkerContext, run_worker};

pub(super) const INSPECTION_QUEUE_CAPACITY: usize = 32;

#[derive(Debug)]
pub struct DecodeInspectionActor {
    pub(in crate::decode_inspection) handle: DecodeInspectionHandle,
    join_handles: Vec<JoinHandle<()>>,
    technical_observer: Option<TechnicalObservationActor>,
    profiled: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
struct DecodeInspectorContract {
    provider_id: Arc<str>,
    provider_version: Arc<str>,
    proxy_variant_key: Arc<str>,
    supports_original_raw: bool,
    supported_original_raster_extensions: Arc<[String]>,
}

type SpawnedInspectionThreads = (Arc<[SyncSender<Message>]>, Vec<JoinHandle<()>>);

#[derive(Copy, Clone)]
struct InspectionThreadContext<'a> {
    catalog: &'a CatalogHandle,
    cache: Option<&'a ContentAddressedStore>,
    technical_handle: Option<&'a TechnicalObservationHandle>,
    embedded_preview_sink: Option<&'a Arc<dyn EmbeddedPreviewSink>>,
    state: &'a Arc<Mutex<DecodeInspectionState>>,
    profiled: bool,
    queue_capacity: usize,
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
    inspectors: Vec<I>,
    context: InspectionThreadContext<'_>,
) -> Result<SpawnedInspectionThreads, DecodeInspectionError> {
    let worker_count = inspectors.len();
    let base_queue_capacity = context.queue_capacity / worker_count;
    let workers_with_extra_slot = context.queue_capacity % worker_count;
    let mut senders = Vec::with_capacity(worker_count);
    let mut join_handles = Vec::with_capacity(worker_count);
    for (worker_index, inspector) in inspectors.into_iter().enumerate() {
        let worker_queue_capacity =
            base_queue_capacity + usize::from(worker_index < workers_with_extra_slot);
        let (sender, receiver) = mpsc::sync_channel(worker_queue_capacity);
        let worker_catalog = context.catalog.clone();
        let worker_cache = context.cache.cloned();
        let worker_technical_handle = context.technical_handle.cloned();
        let worker_embedded_preview_sink = context.embedded_preview_sink.cloned();
        let worker_state = Arc::clone(context.state);
        let profiled = context.profiled;
        let worker_name = if worker_count == 1 {
            "shadow-decode-inspector".to_owned()
        } else {
            format!("shadow-decode-inspector-{worker_index}")
        };
        let spawn_result = thread::Builder::new().name(worker_name).spawn(move || {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                run_worker(
                    inspector,
                    &receiver,
                    WorkerContext {
                        catalog: &worker_catalog,
                        cache: worker_cache.as_ref(),
                        technical_observer: worker_technical_handle.as_ref(),
                        embedded_preview_sink: worker_embedded_preview_sink.as_deref(),
                        state: &worker_state,
                        profiled,
                    },
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
                context
                    .state
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

    pub(in crate::decode_inspection) fn spawn_inner_with_capacity(
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

    pub(super) fn spawn_many_inner<I: DecodeInspector>(
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
            inspectors,
            InspectionThreadContext {
                catalog,
                cache,
                technical_handle: technical_handle.as_ref(),
                embedded_preview_sink: embedded_preview_sink.as_ref(),
                state: &state,
                profiled,
                queue_capacity,
            },
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

impl Drop for DecodeInspectionActor {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

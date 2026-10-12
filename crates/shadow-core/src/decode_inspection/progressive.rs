//! Separate quick source/embedded-preview work from deterministic proxy refinement.
//!
//! Both stages own independent providers. The bounded handoff holds request metadata and
//! completion senders only: camera bytes live in the session sink, and developed pixels
//! never wait in a queue. One accepted request reaches exactly one terminal result.

use std::{
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::AtomicUsize,
        mpsc::{self, SyncSender},
    },
    thread::{self, JoinHandle},
};

use shadow_cache::ContentAddressedStore;
use shadow_catalog::CatalogHandle;

use crate::{
    performance::DecodePerformance,
    technical_observation::{
        TechnicalObservationActor, TechnicalObservationHandle,
        technical_analysis_preprocessing_version,
    },
};

use super::{
    contract::{
        DecodeInspectionError, DecodeInspectionOutcome, DecodeInspectionSummary, DecodeInspector,
        EmbeddedPreviewSink, PreviewCacheOutcome,
    },
    pool::actor::decode_inspector_contract,
    runtime_state::DecodeInspectionState,
    submission::{DecodeInspectionHandle, InspectionMessage, Message},
    visual_cache::{
        VisualCacheContext, cache_embedded_preview, cache_generated_preview, cancelled_outcome,
        reuse_generated_proxy,
    },
    worker::{WorkerContext, inspect_source_and_record},
};

const QUICK_QUEUE_CAPACITY: usize = 32;
// Scheduling policy has its own owner; workers retain source/publication semantics.
mod refinement_queue;
use refinement_queue::RefinementQueue;
pub use refinement_queue::{MAX_VISIBLE_REFINEMENTS, RefinementPriority};

struct RefinementJob {
    inspection: InspectionMessage,
    provider_id: String,
    provider_version: String,
    fallback: Option<PreviewCacheOutcome>,
}

struct Services {
    catalog: CatalogHandle,
    cache: ContentAddressedStore,
    state: Arc<Mutex<DecodeInspectionState>>,
    observer: TechnicalObservationHandle,
    sink: Arc<dyn EmbeddedPreviewSink>,
}

impl Services {
    fn finish(
        &self,
        inspection: InspectionMessage,
        result: Result<DecodeInspectionOutcome, DecodeInspectionError>,
    ) {
        self.state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .summary
            .record(&result);
        let _ = inspection.response.send(result);
    }

    fn worker_context(&self) -> WorkerContext<'_> {
        WorkerContext {
            catalog: &self.catalog,
            cache: Some(&self.cache),
            technical_observer: Some(&self.observer),
            embedded_preview_sink: Some(self.sink.as_ref()),
            state: &self.state,
            profiled: false,
        }
    }

    fn visual_context<'a>(&'a self, job: &'a RefinementJob) -> VisualCacheContext<'a> {
        VisualCacheContext {
            catalog: &self.catalog,
            cache: &self.cache,
            technical_observer: Some(&self.observer),
            embedded_preview_sink: Some(self.sink.as_ref()),
            request: &job.inspection.request,
            provider_id: &job.provider_id,
            provider_version: &job.provider_version,
            cancellation: &job.inspection.cancellation,
        }
    }
}

/// A session-owned staged import pool. Shutdown drains quick admission first, then refinement.
pub struct ProgressiveInspectionPool {
    handle: DecodeInspectionHandle,
    quick_workers: Vec<JoinHandle<()>>,
    refinement_workers: Vec<JoinHandle<()>>,
    queue: Arc<RefinementQueue>,
    observer: Option<TechnicalObservationActor>,
}

impl std::fmt::Debug for ProgressiveInspectionPool {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("ProgressiveInspectionPool")
            .field("quick_workers", &self.quick_workers.len())
            .field("refinement_workers", &self.refinement_workers.len())
            .finish_non_exhaustive()
    }
}

impl ProgressiveInspectionPool {
    /// Starts independent quick and refinement providers sharing one guarded publication path.
    ///
    /// # Errors
    /// Returns an error if construction, provider contracts, cache, or worker startup fails.
    pub fn spawn<I, F, E>(
        catalog: CatalogHandle,
        quick_count: usize,
        refinement_count: usize,
        factory: F,
        cache_root: &Path,
        sink: Arc<dyn EmbeddedPreviewSink>,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        Self::spawn_with_priority(
            catalog,
            quick_count,
            refinement_count,
            factory,
            cache_root,
            sink,
            RefinementPriority::default(),
        )
    }

    /// Starts the same pipeline with an import-owned viewport priority handle.
    ///
    /// # Errors
    /// Returns the same provider, worker, or cache failures as [`Self::spawn`].
    #[allow(clippy::too_many_arguments)]
    pub fn spawn_with_priority<I, F, E>(
        catalog: CatalogHandle,
        quick_count: usize,
        refinement_count: usize,
        mut factory: F,
        cache_root: &Path,
        sink: Arc<dyn EmbeddedPreviewSink>,
        priority: RefinementPriority,
    ) -> Result<Self, DecodeInspectionError>
    where
        I: DecodeInspector,
        F: FnMut(usize) -> Result<I, E>,
        E: std::fmt::Display,
    {
        if quick_count == 0 || refinement_count == 0 {
            return Err(DecodeInspectionError::InvalidWorkerCount);
        }
        let mut inspectors = (0..quick_count + refinement_count)
            .map(|worker_index| {
                factory(worker_index).map_err(|error| DecodeInspectionError::WorkerFactory {
                    worker_index,
                    message: error.to_string(),
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let contract = decode_inspector_contract(&inspectors[0]);
        for (worker_index, inspector) in inspectors.iter().enumerate().skip(1) {
            if decode_inspector_contract(inspector) != contract {
                return Err(DecodeInspectionError::WorkerContractMismatch { worker_index });
            }
        }
        let refiners = inspectors.split_off(quick_count);
        let cache = ContentAddressedStore::open(cache_root)?;
        let observer = TechnicalObservationActor::spawn_with_store(catalog.clone(), cache.clone())?;
        let state = Arc::new(Mutex::new(DecodeInspectionState::default()));
        let services = Arc::new(Services {
            catalog,
            cache,
            state: Arc::clone(&state),
            observer: observer.handle(),
            sink,
        });
        let queue = Arc::new(RefinementQueue::new(priority));
        let mut pool = Self {
            handle: DecodeInspectionHandle {
                senders: Arc::from([]),
                next_worker: Arc::new(AtomicUsize::new(0)),
                state,
                provider_id: contract.provider_id,
                provider_version: contract.provider_version,
                proxy_variant_key: contract.proxy_variant_key,
                technical_preprocessing_version: Some(Arc::from(
                    technical_analysis_preprocessing_version(),
                )),
                supports_original_raw: contract.supports_original_raw,
                supported_original_raster_extensions: contract.supported_original_raster_extensions,
                caches_previews: true,
                profiled: false,
            },
            quick_workers: Vec::new(),
            refinement_workers: Vec::new(),
            queue,
            observer: Some(observer),
        };
        for (index, inspector) in refiners.into_iter().enumerate() {
            let services = Arc::clone(&services);
            let queue = Arc::clone(&pool.queue);
            pool.refinement_workers.push(
                thread::Builder::new()
                    .name(format!("shadow-import-refinement-{index}"))
                    .spawn(move || {
                        guarded_worker(&services, &queue, || {
                            run_refinement(inspector, &queue, &services)
                        })
                    })
                    .map_err(DecodeInspectionError::WorkerStart)?,
            );
        }
        let mut senders: Vec<SyncSender<Message>> = Vec::new();
        for (index, inspector) in inspectors.into_iter().enumerate() {
            let (sender, receiver) = mpsc::sync_channel(QUICK_QUEUE_CAPACITY.div_ceil(quick_count));
            let services = Arc::clone(&services);
            let queue = Arc::clone(&pool.queue);
            let worker = thread::Builder::new()
                .name(format!("shadow-import-quick-{index}"))
                .spawn(move || {
                    guarded_worker(&services, &queue, || {
                        let mut inspector = inspector;
                        while let Ok(message) = receiver.recv() {
                            match message {
                                Message::Inspect(inspection) => {
                                    run_quick(&mut inspector, inspection, &queue, &services)
                                }
                                Message::Shutdown(response) => {
                                    let _ = response.send(DecodePerformance::default());
                                    break;
                                }
                            }
                        }
                    })
                })
                .map_err(DecodeInspectionError::WorkerStart)?;
            senders.push(sender);
            pool.quick_workers.push(worker);
            // A partial spawn still owns every already-started worker's shutdown channel.
            pool.handle.senders = senders.clone().into();
        }
        Ok(pool)
    }

    #[must_use]
    pub fn handle(&self) -> DecodeInspectionHandle {
        self.handle.clone()
    }

    /// Drains both stages and returns exact terminal accounting.
    ///
    /// # Errors
    /// Returns an error if a worker or detached technical observer failed.
    pub fn shutdown_with_summary(
        mut self,
    ) -> Result<DecodeInspectionSummary, DecodeInspectionError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<DecodeInspectionSummary, DecodeInspectionError> {
        self.handle
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .stopping = true;
        let mut unavailable = false;
        for sender in self.handle.senders.iter() {
            let (sender_ack, _) = mpsc::sync_channel(1);
            if sender.send(Message::Shutdown(sender_ack)).is_err() {
                unavailable = true;
            }
        }
        for worker in self.quick_workers.drain(..) {
            unavailable |= worker.join().is_err();
        }
        self.handle.senders = Arc::from([]);
        self.queue.close();
        for worker in self.refinement_workers.drain(..) {
            unavailable |= worker.join().is_err();
        }
        if let Some(observer) = self.observer.take() {
            observer.shutdown()?;
        }
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
        Ok(state.summary)
    }
}

impl Drop for ProgressiveInspectionPool {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

fn guarded_worker(services: &Services, queue: &RefinementQueue, run: impl FnOnce()) {
    if std::panic::catch_unwind(std::panic::AssertUnwindSafe(run)).is_err() {
        let mut state = services
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        state.worker_panicked = true;
        state.stopping = true;
        queue.close();
    }
}

fn run_quick(
    inspector: &mut impl DecodeInspector,
    inspection: InspectionMessage,
    queue: &RefinementQueue,
    services: &Services,
) {
    let mut performance = DecodePerformance::default();
    let admission = inspect_source_and_record(
        inspector,
        &inspection.request,
        &inspection.cancellation,
        services.worker_context(),
        &mut performance,
    );
    let (provider_id, provider_version) = match admission {
        Ok(DecodeInspectionOutcome::Recorded {
            provider_id,
            provider_version,
            ..
        }) => (provider_id, provider_version),
        other => {
            services.finish(inspection, other);
            return;
        }
    };
    let mut job = RefinementJob {
        inspection,
        provider_id,
        provider_version,
        fallback: None,
    };
    if let Some(preview) =
        reuse_generated_proxy(inspector, services.visual_context(&job), &mut performance)
    {
        let result = Ok(DecodeInspectionOutcome::Recorded {
            provider_id: job.provider_id,
            provider_version: job.provider_version,
            preview,
        });
        services.finish(job.inspection, result);
        return;
    }
    job.fallback = cache_embedded_preview(
        inspector,
        services.visual_context(&job),
        &services.state,
        &mut performance,
    );
    if let Some(PreviewCacheOutcome::Discarded(reason)) = &job.fallback {
        let result = Ok(DecodeInspectionOutcome::Discarded(*reason));
        services.finish(job.inspection, result);
        return;
    }
    if let Err(job) = queue.push(job) {
        let result = if job.inspection.cancellation.is_cancelled() {
            Ok(cancelled_outcome())
        } else {
            Err(DecodeInspectionError::WorkerUnavailable)
        };
        services.finish(job.inspection, result);
    }
}

fn run_refinement(
    mut inspector: impl DecodeInspector,
    queue: &RefinementQueue,
    services: &Services,
) {
    while let Some(mut job) = queue.pop() {
        let fallback = job.fallback.take();
        let preview = cache_generated_preview(
            &mut inspector,
            services.visual_context(&job),
            &services.state,
            &mut DecodePerformance::default(),
            fallback,
        );
        let result = Ok(DecodeInspectionOutcome::Recorded {
            provider_id: job.provider_id,
            provider_version: job.provider_version,
            preview,
        });
        services.finish(job.inspection, result);
    }
}

#[cfg(test)]
mod tests;

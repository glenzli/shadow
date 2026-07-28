//! Independent-provider pool construction and worker-count policy.

use std::{path::PathBuf, sync::Arc, thread};

use shadow_cache::ContentAddressedStore;
use shadow_catalog::CatalogHandle;

use super::super::contract::{
    DecodeInspectionError, DecodeInspectionSummary, DecodeInspectionTerminal, DecodeInspector,
    EmbeddedPreviewSink,
};
use super::super::submission::DecodeInspectionHandle;
use super::actor::{DecodeInspectionActor, INSPECTION_QUEUE_CAPACITY};

pub(in crate::decode_inspection) const MIN_RECOMMENDED_INSPECTION_WORKERS: usize = 2;
pub(in crate::decode_inspection) const MAX_RECOMMENDED_INSPECTION_WORKERS: usize = 4;
const LOGICAL_CPUS_PER_INSPECTION_WORKER: usize = 6;

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

    pub(in crate::decode_inspection) fn spawn_inner<I, F, E>(
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

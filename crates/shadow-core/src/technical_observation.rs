//! Background, version-bound technical observations over cached display JPEGs.

use std::{
    fmt::Write as _,
    path::PathBuf,
    sync::mpsc::{self, Receiver, Sender, SyncSender},
    thread::{self, JoinHandle},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_ai::{
    DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, TechnicalObservationError as AiError,
    observe_display_luma,
};
use shadow_bridge::{
    BridgeError, JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX, decode_jpeg_display_luma,
};
use shadow_cache::{BlobDigest, CacheError, ContentAddressedStore};
use shadow_catalog::{
    CachedArtifactRecord, CatalogError, CatalogHandle, RecordTechnicalObservation,
    RecordTechnicalObservationStatus,
};
use shadow_domain::{PreviewCodec, RepresentationId};
use thiserror::Error;

use crate::performance::{TechnicalPerformance, measure_if};

const OBSERVATION_QUEUE_CAPACITY: usize = 16;

/// Fixed longest edge for first-generation display-proxy observations.
///
/// This is part of the persisted preprocessing identity. Changing it creates a
/// new observation revision and must never silently reinterpret old metrics.
pub const TECHNICAL_ANALYSIS_MAX_EDGE: u32 = 512;

/// Returns the exact preprocessing revision required by this build.
pub fn technical_analysis_preprocessing_version() -> String {
    format!(
        "{JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX}:max-edge-{TECHNICAL_ANALYSIS_MAX_EDGE}"
    )
}

/// Result of one display-proxy observation request.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum TechnicalObservationOutcome {
    Recorded,
    DiscardedStaleInput,
    UnsupportedCodec(PreviewCodec),
}

/// Failures from cache verification, bounded JPEG preprocessing, analysis, or
/// the exact-input Catalog commit.
#[derive(Debug, Error)]
pub enum TechnicalObservationError {
    #[error("technical observation only supports BLAKE3 cache blobs, got {0:?}")]
    UnsupportedBlobAlgorithm(String),
    #[error("verified cache blob length is {actual}; Catalog expected {expected}")]
    BlobLengthMismatch { expected: u64, actual: u64 },
    #[error("cache operation failed: {0}")]
    Cache(#[from] CacheError),
    #[error("JPEG display-luma preprocessing failed: {0}")]
    Bridge(#[from] BridgeError),
    #[error("technical observation failed: {0}")]
    Observation(#[from] AiError),
    #[error("Catalog operation failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("cannot start technical observation worker: {0}")]
    WorkerStart(#[source] std::io::Error),
    #[error("technical observation worker is unavailable")]
    WorkerUnavailable,
    #[error("technical observation worker panicked")]
    WorkerPanicked,
}

/// Owns the single background analysis worker.
#[derive(Debug)]
pub struct TechnicalObservationActor {
    handle: TechnicalObservationHandle,
    join_handle: Option<JoinHandle<()>>,
    profiled: bool,
}

/// Cloneable bounded-queue handle for technical analysis requests.
#[derive(Debug, Clone)]
pub struct TechnicalObservationHandle {
    sender: SyncSender<Message>,
}

/// Completion handle for one observation request.
#[derive(Debug)]
pub struct TechnicalObservationTicket {
    receiver: Receiver<Result<TechnicalObservationOutcome, TechnicalObservationError>>,
}

enum Message {
    Observe(Box<CachedArtifactRecord>, Completion),
    ObservePreferred(
        RepresentationId,
        shadow_catalog::RepresentationFingerprint,
        Completion,
    ),
    Shutdown(SyncSender<Result<TechnicalPerformance, TechnicalObservationError>>),
}

enum Completion {
    Ticket(Sender<Result<TechnicalObservationOutcome, TechnicalObservationError>>),
    Detached,
}

impl TechnicalObservationActor {
    /// Starts one bounded, serialized worker over a content-addressed cache.
    ///
    /// # Errors
    ///
    /// Returns an error when the cache root cannot be opened or the worker
    /// thread cannot be created.
    pub fn spawn(
        catalog: CatalogHandle,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, TechnicalObservationError> {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_with_store_mode(catalog, cache, false)
    }

    /// Starts one bounded observation worker with monotonic phase profiling.
    ///
    /// Profiling is explicit so the default worker performs no per-job clock
    /// reads. Retrieve the aggregate with [`Self::shutdown_with_performance`].
    ///
    /// # Errors
    ///
    /// Returns an error when the cache root cannot be opened or the worker
    /// thread cannot be created.
    pub fn spawn_profiled(
        catalog: CatalogHandle,
        cache_root: impl Into<PathBuf>,
    ) -> Result<Self, TechnicalObservationError> {
        let cache = ContentAddressedStore::open(cache_root)?;
        Self::spawn_with_store_mode(catalog, cache, true)
    }

    pub(crate) fn spawn_with_store(
        catalog: CatalogHandle,
        cache: ContentAddressedStore,
    ) -> Result<Self, TechnicalObservationError> {
        Self::spawn_with_store_mode(catalog, cache, false)
    }

    pub(crate) fn spawn_with_store_profiled(
        catalog: CatalogHandle,
        cache: ContentAddressedStore,
    ) -> Result<Self, TechnicalObservationError> {
        Self::spawn_with_store_mode(catalog, cache, true)
    }

    fn spawn_with_store_mode(
        catalog: CatalogHandle,
        cache: ContentAddressedStore,
        profiled: bool,
    ) -> Result<Self, TechnicalObservationError> {
        let (sender, receiver) = mpsc::sync_channel(OBSERVATION_QUEUE_CAPACITY);
        let join_handle = thread::Builder::new()
            .name("shadow-technical-observer".to_owned())
            .spawn(move || run_worker(&catalog, &cache, &receiver, profiled))
            .map_err(TechnicalObservationError::WorkerStart)?;
        Ok(Self {
            handle: TechnicalObservationHandle { sender },
            join_handle: Some(join_handle),
            profiled,
        })
    }

    pub fn handle(&self) -> TechnicalObservationHandle {
        self.handle.clone()
    }

    /// Drains all submitted observations, then stops the worker.
    ///
    /// # Errors
    ///
    /// Returns an error if the worker disconnected or panicked.
    pub fn shutdown(mut self) -> Result<(), TechnicalObservationError> {
        self.stop_and_join().map(|_| ())
    }

    /// Drains submitted observations and returns the terminal phase aggregate.
    ///
    /// A worker created by [`Self::spawn`] returns a disabled, empty aggregate;
    /// only [`Self::spawn_profiled`] performs per-job monotonic clock reads.
    ///
    /// # Errors
    ///
    /// Returns an error if the worker disconnected or panicked.
    pub fn shutdown_with_performance(
        mut self,
    ) -> Result<TechnicalPerformance, TechnicalObservationError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<TechnicalPerformance, TechnicalObservationError> {
        let Some(join_handle) = self.join_handle.take() else {
            return Ok(TechnicalPerformance {
                profiled: self.profiled,
                ..TechnicalPerformance::default()
            });
        };
        let (response_sender, response_receiver) = mpsc::sync_channel(0);
        if self
            .handle
            .sender
            .send(Message::Shutdown(response_sender))
            .is_err()
        {
            return match join_handle.join() {
                Ok(()) => Err(TechnicalObservationError::WorkerUnavailable),
                Err(_) => Err(TechnicalObservationError::WorkerPanicked),
            };
        }
        let worker_result = response_receiver.recv();
        if join_handle.join().is_err() {
            return Err(TechnicalObservationError::WorkerPanicked);
        }
        worker_result.map_err(|_| TechnicalObservationError::WorkerUnavailable)?
    }
}

impl Drop for TechnicalObservationActor {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

impl TechnicalObservationHandle {
    /// Queues one exact cached-artifact revision for analysis.
    ///
    /// # Errors
    ///
    /// Returns an error if the worker has already stopped.
    pub fn submit(
        &self,
        artifact: CachedArtifactRecord,
    ) -> Result<TechnicalObservationTicket, TechnicalObservationError> {
        let (response_sender, response_receiver) = mpsc::channel();
        self.sender
            .send(Message::Observe(
                Box::new(artifact),
                Completion::Ticket(response_sender),
            ))
            .map_err(|_| TechnicalObservationError::WorkerUnavailable)?;
        Ok(TechnicalObservationTicket {
            receiver: response_receiver,
        })
    }

    /// Queues whichever exact current artifact the Review selection contract
    /// prefers when the job executes.
    ///
    /// Selecting inside the observation worker lets a migration backfill an old
    /// provider/variant that still outranks the artifact just regenerated by
    /// the active decoder.
    ///
    /// # Errors
    ///
    /// Returns an error if the worker has already stopped.
    pub fn submit_preferred(
        &self,
        representation_id: RepresentationId,
        expected_source: shadow_catalog::RepresentationFingerprint,
    ) -> Result<TechnicalObservationTicket, TechnicalObservationError> {
        let (response_sender, response_receiver) = mpsc::channel();
        self.sender
            .send(Message::ObservePreferred(
                representation_id,
                expected_source,
                Completion::Ticket(response_sender),
            ))
            .map_err(|_| TechnicalObservationError::WorkerUnavailable)?;
        Ok(TechnicalObservationTicket {
            receiver: response_receiver,
        })
    }

    pub(crate) fn submit_preferred_detached(
        &self,
        representation_id: RepresentationId,
        expected_source: shadow_catalog::RepresentationFingerprint,
    ) -> Result<(), TechnicalObservationError> {
        self.sender
            .send(Message::ObservePreferred(
                representation_id,
                expected_source,
                Completion::Detached,
            ))
            .map_err(|_| TechnicalObservationError::WorkerUnavailable)
    }
}

impl TechnicalObservationTicket {
    /// Waits for the exact-input Catalog commit or a deterministic failure.
    ///
    /// # Errors
    ///
    /// Returns the request failure, or an unavailable-worker error after a
    /// disconnect.
    pub fn wait(self) -> Result<TechnicalObservationOutcome, TechnicalObservationError> {
        self.receiver
            .recv()
            .map_err(|_| TechnicalObservationError::WorkerUnavailable)?
    }
}

fn run_worker(
    catalog: &CatalogHandle,
    cache: &ContentAddressedStore,
    receiver: &Receiver<Message>,
    profiled: bool,
) {
    let mut first_unobserved_error = None;
    let mut performance = TechnicalPerformance {
        profiled,
        ..TechnicalPerformance::default()
    };
    while let Ok(message) = receiver.recv() {
        match message {
            Message::Observe(artifact, completion) => {
                let result = measure_if(
                    profiled,
                    &mut performance.technical_observation_total,
                    || observe_cached_artifact(catalog, cache, artifact.as_ref()),
                );
                complete_observation(completion, result, &mut first_unobserved_error);
            }
            Message::ObservePreferred(representation_id, expected_source, completion) => {
                let result = measure_if(
                    profiled,
                    &mut performance.technical_observation_total,
                    || {
                        observe_preferred_artifact(
                            catalog,
                            cache,
                            representation_id,
                            expected_source,
                        )
                    },
                );
                complete_observation(completion, result, &mut first_unobserved_error);
            }
            Message::Shutdown(response) => {
                let result = first_unobserved_error.map_or(Ok(performance), Err);
                let _ = response.send(result);
                break;
            }
        }
    }
}

fn complete_observation(
    completion: Completion,
    result: Result<TechnicalObservationOutcome, TechnicalObservationError>,
    first_unobserved_error: &mut Option<TechnicalObservationError>,
) {
    match completion {
        Completion::Ticket(response) => {
            if let Err(undelivered) = response.send(result)
                && first_unobserved_error.is_none()
                && let Err(error) = undelivered.0
            {
                *first_unobserved_error = Some(error);
            }
        }
        Completion::Detached => {
            if first_unobserved_error.is_none()
                && let Err(error) = result
            {
                *first_unobserved_error = Some(error);
            }
        }
    }
}

fn observe_preferred_artifact(
    catalog: &CatalogHandle,
    cache: &ContentAddressedStore,
    representation_id: RepresentationId,
    expected_source: shadow_catalog::RepresentationFingerprint,
) -> Result<TechnicalObservationOutcome, TechnicalObservationError> {
    let Some(record) = catalog.preferred_cached_artifact(representation_id)? else {
        return Ok(TechnicalObservationOutcome::DiscardedStaleInput);
    };
    if record.source != expected_source {
        return Ok(TechnicalObservationOutcome::DiscardedStaleInput);
    }
    observe_cached_artifact(catalog, cache, &record)
}

fn observe_cached_artifact(
    catalog: &CatalogHandle,
    cache: &ContentAddressedStore,
    record: &CachedArtifactRecord,
) -> Result<TechnicalObservationOutcome, TechnicalObservationError> {
    if record.artifact.codec != PreviewCodec::Jpeg {
        return Ok(TechnicalObservationOutcome::UnsupportedCodec(
            record.artifact.codec,
        ));
    }
    if record.artifact.blob_algorithm != "blake3-256" {
        return Err(TechnicalObservationError::UnsupportedBlobAlgorithm(
            record.artifact.blob_algorithm.clone(),
        ));
    }

    let digest = BlobDigest::from_bytes(record.artifact.blob_digest);
    let bytes = cache.read_verified(digest)?;
    let byte_len = u64::try_from(bytes.len()).unwrap_or(u64::MAX);
    if byte_len != record.artifact.blob_byte_len {
        return Err(TechnicalObservationError::BlobLengthMismatch {
            expected: record.artifact.blob_byte_len,
            actual: byte_len,
        });
    }

    let decoded = decode_jpeg_display_luma(&bytes, TECHNICAL_ANALYSIS_MAX_EDGE)?;
    let input_source_hash = input_source_hash(record);
    let observation = observe_display_luma(DisplayLumaPlane {
        contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
        width: decoded.width,
        height: decoded.height,
        stride: decoded.stride,
        samples: &decoded.samples,
        preprocessing_version: &decoded.preprocessing_version,
        input_source_hash: &input_source_hash,
    })?;
    let status = catalog.record_technical_observation(&RecordTechnicalObservation {
        representation_id: record.representation_id,
        expected_source: record.source,
        expected_artifact: record.artifact.clone(),
        observation,
        observed_at_ms: now_ms(),
    })?;
    Ok(match status {
        RecordTechnicalObservationStatus::Recorded => TechnicalObservationOutcome::Recorded,
        RecordTechnicalObservationStatus::StaleInput => {
            TechnicalObservationOutcome::DiscardedStaleInput
        }
    })
}

fn input_source_hash(record: &CachedArtifactRecord) -> String {
    let mut value = String::with_capacity(record.artifact.blob_algorithm.len() + 65);
    value.push_str(&record.artifact.blob_algorithm);
    value.push(':');
    for byte in record.artifact.blob_digest {
        write!(&mut value, "{byte:02x}").expect("writing into String cannot fail");
    }
    value
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests;

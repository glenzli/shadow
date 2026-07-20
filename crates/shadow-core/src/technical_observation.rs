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

#[cfg(test)]
pub(crate) const TEST_DISPLAY_JPEG: &[u8] = &[
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
    0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0a, 0x07,
    0x07, 0x06, 0x08, 0x0c, 0x0a, 0x0c, 0x0c, 0x0b, 0x0a, 0x0b, 0x0b, 0x0d, 0x0e, 0x12, 0x10, 0x0d,
    0x0e, 0x11, 0x0e, 0x0b, 0x0b, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0c, 0x0f,
    0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x02,
    0x00, 0x02, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xff, 0xc4, 0x00, 0x1d,
    0x10, 0x00, 0x02, 0x01, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x06, 0x03, 0x04, 0x05, 0x07, 0x00, 0x12, 0x62, 0xff, 0xda, 0x00, 0x08, 0x01,
    0x01, 0x00, 0x00, 0x3f, 0x00, 0x41, 0xe2, 0xfa, 0x1b, 0x59, 0xd3, 0x8d, 0x62, 0x55, 0x75, 0xdc,
    0x4d, 0x55, 0x6d, 0x28, 0x80, 0xa3, 0x09, 0x6c, 0x00, 0x1d, 0x07, 0x8e, 0x7f, 0xff, 0xd9,
];

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
mod tests {
    use shadow_catalog::{
        CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact,
        RecordCachedArtifactStatus, RegisterAsset, TechnicalObservationRevision,
    };
    use shadow_domain::{
        AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, RepresentationKind,
    };

    use super::*;

    #[test]
    fn worker_persists_observation_for_the_review_preferred_artifact() {
        let ObservationFixture {
            root,
            catalog_actor,
            catalog,
            cache_root,
            source,
            representation_id,
            generated_artifact,
            preferred_artifact,
        } = observation_fixture();
        let observer = TechnicalObservationActor::spawn_profiled(catalog.clone(), &cache_root)
            .expect("start profiled technical observer");
        let outcome = observer
            .handle()
            .submit_preferred(representation_id, source)
            .expect("submit observation")
            .wait()
            .expect("observe JPEG");
        assert_eq!(outcome, TechnicalObservationOutcome::Recorded);
        let performance = observer
            .shutdown_with_performance()
            .expect("stop profiled technical observer");
        assert!(performance.profiled);
        assert_eq!(performance.technical_observation_total.samples, 1);

        let revision =
            TechnicalObservationRevision::current(technical_analysis_preprocessing_version());
        let persisted = catalog
            .technical_observation(representation_id, source, &preferred_artifact, &revision)
            .expect("read observation")
            .expect("observation exists");
        assert_eq!(persisted.observation.input.width, 2);
        assert_eq!(persisted.observation.input.height, 2);
        assert_eq!(
            persisted.observation.input.preprocessing_version,
            revision.preprocessing_version
        );
        assert!(
            catalog
                .technical_observation(representation_id, source, &generated_artifact, &revision)
                .expect("read lower-priority observation")
                .is_none()
        );

        finish_fixture(catalog_actor, root);
    }

    #[test]
    fn detached_observation_failure_is_reported_on_shutdown() {
        let ObservationFixture {
            root,
            catalog_actor,
            catalog,
            cache_root,
            source,
            representation_id,
            mut preferred_artifact,
            ..
        } = observation_fixture();
        let observer = TechnicalObservationActor::spawn(catalog, cache_root)
            .expect("start failure-reporting observer");
        preferred_artifact.blob_digest = [9; 32];
        observer
            .handle()
            .sender
            .send(Message::Observe(
                Box::new(CachedArtifactRecord {
                    representation_id,
                    source,
                    artifact: preferred_artifact,
                }),
                Completion::Detached,
            ))
            .expect("submit detached missing cache blob");
        assert!(matches!(
            observer.shutdown(),
            Err(TechnicalObservationError::Cache(_))
        ));

        finish_fixture(catalog_actor, root);
    }

    struct ObservationFixture {
        root: PathBuf,
        catalog_actor: CatalogActor,
        catalog: CatalogHandle,
        cache_root: PathBuf,
        source: shadow_catalog::RepresentationFingerprint,
        representation_id: RepresentationId,
        generated_artifact: CachedArtifact,
        preferred_artifact: CachedArtifact,
    }

    fn observation_fixture() -> ObservationFixture {
        let root = std::env::temp_dir().join(format!(
            "shadow-technical-observer-{}-{}",
            std::process::id(),
            shadow_domain::RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create technical observation fixture");
        let catalog_actor =
            CatalogActor::spawn(&root.join("catalog.sqlite")).expect("start Catalog actor");
        let catalog = catalog_actor.handle();
        let source = shadow_catalog::RepresentationFingerprint {
            byte_len: 4_096,
            modified_at_ms: Some(123),
        };
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/input.dng".to_vec(),
                    "/photos/input.dng",
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register source");
        let cache_root = root.join("cache");
        let cache = ContentAddressedStore::open(&cache_root).expect("open cache");
        let blob = cache.put(TEST_DISPLAY_JPEG).expect("store JPEG");
        let generated_artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: "test:grid-jpeg-v1".into(),
            generator_id: "test".into(),
            generator_version: "1".into(),
            provider_preview_id: None,
            blob_algorithm: blob.digest.algorithm().into(),
            blob_digest: *blob.digest.as_bytes(),
            blob_byte_len: blob.byte_len,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 2,
                height: 2,
            },
            bits_per_channel: 8,
            channels: 1,
            created_at_ms: 200,
        };
        assert_eq!(
            catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id: registered.representation_id,
                    expected_source: source,
                    artifact: generated_artifact.clone(),
                })
                .expect("record cached JPEG"),
            RecordCachedArtifactStatus::Recorded
        );
        let preferred_artifact = CachedArtifact {
            role: CachedArtifactRole::EmbeddedPreview,
            variant_key: "legacy-provider".into(),
            generator_id: "legacy-provider".into(),
            generator_version: "1".into(),
            provider_preview_id: Some(7),
            created_at_ms: 201,
            ..generated_artifact.clone()
        };
        assert_eq!(
            catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id: registered.representation_id,
                    expected_source: source,
                    artifact: preferred_artifact.clone(),
                })
                .expect("record old preferred JPEG"),
            RecordCachedArtifactStatus::Recorded
        );
        ObservationFixture {
            root,
            catalog_actor,
            catalog,
            cache_root,
            source,
            representation_id: registered.representation_id,
            generated_artifact,
            preferred_artifact,
        }
    }

    fn finish_fixture(catalog_actor: CatalogActor, root: PathBuf) {
        catalog_actor.shutdown().expect("stop Catalog actor");
        std::fs::remove_dir_all(root).expect("remove technical observation fixture");
    }
}

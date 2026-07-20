use std::{
    path::{Path, PathBuf},
    sync::mpsc::{self, Receiver, Sender, SyncSender},
    thread::{self, JoinHandle},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{
    CatalogError, CatalogHandle, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};
use shadow_domain::{DecoderSnapshot, RepresentationId};
use thiserror::Error;

const INSPECTION_QUEUE_CAPACITY: usize = 32;

/// Provider-neutral operation used by the background decode worker.
///
/// Implementations adapt `LibRaw`, a private vendor SDK bridge, a DNG converter,
/// or a test double without exposing provider types to the scheduler.
pub trait DecodeInspector: Send + 'static {
    /// Inspects one source and returns an owned, provider-neutral snapshot.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic suitable for the background job log.
    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String>;
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
    },
    Discarded(DecodeInspectionDiscardReason),
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
}

#[derive(Debug, Clone)]
pub struct DecodeInspectionHandle {
    sender: SyncSender<Message>,
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
        let (sender, receiver) = mpsc::sync_channel(INSPECTION_QUEUE_CAPACITY);
        let join_handle = thread::Builder::new()
            .name("shadow-decode-inspector".to_owned())
            .spawn(move || run_worker(&catalog, inspector, &receiver))
            .map_err(DecodeInspectionError::WorkerStart)?;
        Ok(Self {
            handle: DecodeInspectionHandle { sender },
            join_handle: Some(join_handle),
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
        let Some(join_handle) = self.join_handle.take() else {
            return Ok(());
        };
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
    }
}

impl Drop for DecodeInspectionActor {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

impl DecodeInspectionHandle {
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
    receiver: &Receiver<Message>,
) {
    while let Ok(message) = receiver.recv() {
        match message {
            Message::Inspect(request, response) => {
                let result = inspect_and_record(catalog, &mut inspector, &request);
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
        RecordDecodeSnapshotStatus::Recorded => DecodeInspectionOutcome::Recorded {
            provider_id,
            provider_version,
        },
        RecordDecodeSnapshotStatus::StaleSource => {
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::CatalogChanged)
        }
    })
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
        RawMetadataSnapshot, RepresentationKind,
    };

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
                provider_id: "test-decoder".into(),
                provider_version: "1".into(),
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
                id: "test-decoder".into(),
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

//! Opt-in Infer Runtime execution for one already decoded RAW foundation.
//!
//! The transaction deliberately begins with Shadow's local alias/store lookup.
//! Only a clean miss constructs an authenticated Infer client, creates a Job,
//! and transfers the two owner-only file handles. Infer owns model execution;
//! Shadow independently verifies and publishes the returned `.shadowrawf`,
//! records a rebuildable alias, and continues to own stale-result arbitration.

mod alias_store;

use std::{
    fs::{File, OpenOptions},
    path::{Path, PathBuf},
    thread,
    time::Duration,
};

use shadow_ai::{
    ArtifactHashAlgorithm, CancellationToken, GeneratedArtifactReference,
    InferRawFoundationDecoderIdentity, InferRawFoundationPriority, InferRawFoundationProvider,
    InferRawFoundationRequest, InferRawFoundationResult, InferRawFoundationSource,
    InferRawFoundationStaging, InferRuntimeClient, InferRuntimeClientError,
    RAW_FOUNDATION_ENCODING_VERSION, RAW_FOUNDATION_MEDIA_TYPE,
    RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256, RAWNIND_FOUNDATION_PACKAGE_SHA256,
    RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256, RasterExtent, RawFoundationArtifact,
    RawFoundationArtifactError, RawFoundationMaterializationDisposition, RawFoundationProvenance,
    RawFoundationSourceProvenance, RuntimeProgress, RuntimeProgressSink,
};
use shadow_cache::{
    FoundationArtifactPublicationStatus, FoundationArtifactReader, FoundationArtifactStore,
    FoundationArtifactStoreError, FoundationArtifactVerification,
};
use thiserror::Error;

use self::alias_store::{InferCacheAliasError, InferCacheAliasExpectation, InferCacheAliasStore};
use crate::isolated_proxy::IsolatedRawFrameStaging;

const IMPLEMENTATION_REVISION: &str = "rawnind-public-bayer-foundation-ort127-exp1";
const EXECUTION_POLL_INTERVAL: Duration = Duration::from_millis(25);
const PLANNING_PROGRESS: u16 = 1_000;
const RUNNING_PROGRESS: u16 = 4_000;
const PUBLISHING_PROGRESS: u16 = 9_700;

#[derive(Debug, Clone)]
pub(super) struct InferRawFoundationMaterializer {
    base_url_override: Option<String>,
    credential_file: PathBuf,
    aliases: InferCacheAliasStore,
}

#[derive(Debug)]
pub(super) struct InferMaterializedFoundation {
    pub(super) descriptor: RawFoundationArtifact,
    pub(super) path: PathBuf,
    pub(super) disposition: RawFoundationMaterializationDisposition,
    pub(super) verified_reader: Option<FoundationArtifactReader>,
}

#[derive(Debug)]
pub(super) enum InferMaterializationOutcome {
    Ready(Box<InferMaterializedFoundation>),
    Cancelled,
}

impl InferRawFoundationMaterializer {
    pub(super) fn new(
        base_url_override: Option<String>,
        credential_file: PathBuf,
        foundation_store_root: &Path,
    ) -> Self {
        Self {
            base_url_override,
            credential_file,
            aliases: InferCacheAliasStore::new(foundation_store_root),
        }
    }

    pub(super) fn probe_client(&self) -> Result<(), InferMaterializationError> {
        self.client().map(|_| ())
    }

    pub(super) fn resolve_cached(
        &self,
        store: &FoundationArtifactStore,
        source_sha256: &str,
        source_size_bytes: u64,
        staging: &IsolatedRawFrameStaging,
    ) -> Result<Option<InferMaterializedFoundation>, InferMaterializationError> {
        let source_revision = source_revision(source_sha256, staging);
        let expected =
            alias_expectation(&source_revision, source_sha256, source_size_bytes, staging);
        let Some(reader) = self.aliases.lookup(store, &expected)? else {
            return Ok(None);
        };
        let verification = reader.verification().clone();
        let descriptor = descriptor_from_verification(&verification)?;
        Ok(Some(InferMaterializedFoundation {
            path: verification.path,
            descriptor,
            disposition: RawFoundationMaterializationDisposition::ReusedVerified,
            verified_reader: Some(reader),
        }))
    }

    pub(super) fn materialize(
        &self,
        store: &FoundationArtifactStore,
        source_sha256: &str,
        source_size_bytes: u64,
        staging: &IsolatedRawFrameStaging,
        cancellation: &CancellationToken,
        progress: &dyn RuntimeProgressSink,
    ) -> Result<InferMaterializationOutcome, InferMaterializationError> {
        progress.publish(RuntimeProgress {
            phase_code: "planning".into(),
            completed_basis_points: PLANNING_PROGRESS,
        });
        if cancellation.is_cancelled() {
            return Ok(InferMaterializationOutcome::Cancelled);
        }
        if let Some(cached) =
            self.resolve_cached(store, source_sha256, source_size_bytes, staging)?
        {
            return Ok(InferMaterializationOutcome::Ready(Box::new(cached)));
        }

        let source_revision = source_revision(source_sha256, staging);
        let expected =
            alias_expectation(&source_revision, source_sha256, source_size_bytes, staging);
        let request = request(&source_revision, source_sha256, source_size_bytes, staging)?;
        let provider = self.client()?;
        let partial_key = blake3::hash(source_revision.as_bytes())
            .to_hex()
            .to_string();
        let partial_path = store.allocate_partial_path(&partial_key)?;
        let mut partial = OwnedPartial::new(store.clone(), partial_path.clone());
        let output = create_owner_output(&partial_path)?;
        let input = staging.try_clone_sample()?;

        let grant = provider.begin_raw_foundation(&request)?;
        let job = grant.job();
        if cancellation.is_cancelled() {
            let _ = provider.cancel_raw_foundation(&job);
            return Ok(InferMaterializationOutcome::Cancelled);
        }
        let lease = match provider.register_raw_foundation_handles(grant, &input, &output) {
            Ok(lease) => lease,
            Err(error) => {
                let _ = provider.cancel_raw_foundation(&job);
                return Err(error.into());
            }
        };
        drop(input);
        drop(output);
        progress.publish(RuntimeProgress {
            phase_code: "running".into(),
            completed_basis_points: RUNNING_PROGRESS,
        });
        let execution = execute_with_cancellation(&provider, lease, cancellation)?;
        let Some(result) = execution else {
            return Ok(InferMaterializationOutcome::Cancelled);
        };

        progress.publish(RuntimeProgress {
            phase_code: "publishing".into(),
            completed_basis_points: PUBLISHING_PROGRESS,
        });
        // From this boundary the store owns validation-failure retention and
        // crash recovery. Earlier transport/cancellation failures still
        // discard the incomplete output through the guard.
        partial.preserve();
        let publication =
            store.publish_verified_partial(&result.artifact.cache_key_sha256, partial.path())?;
        validate_result(&result, &publication.verification, &expected)?;
        let descriptor = descriptor_from_verification(&publication.verification)?;
        self.aliases.record(&expected, &publication.verification)?;
        let disposition = match publication.status {
            FoundationArtifactPublicationStatus::Published => {
                RawFoundationMaterializationDisposition::Published
            }
            FoundationArtifactPublicationStatus::ReusedExisting => {
                RawFoundationMaterializationDisposition::ReusedConcurrent
            }
        };
        Ok(InferMaterializationOutcome::Ready(Box::new(
            InferMaterializedFoundation {
                descriptor,
                path: publication.path,
                disposition,
                verified_reader: None,
            },
        )))
    }

    fn client(&self) -> Result<InferRuntimeClient, InferMaterializationError> {
        InferRuntimeClient::from_credential_file_with_discovery(
            self.base_url_override.as_deref(),
            &self.credential_file,
        )
        .map_err(Into::into)
    }
}

fn request(
    source_revision: &str,
    source_sha256: &str,
    source_size_bytes: u64,
    staging: &IsolatedRawFrameStaging,
) -> Result<InferRawFoundationRequest, InferMaterializationError> {
    let descriptor = staging.descriptor();
    let staging = InferRawFoundationStaging::new(
        descriptor.width,
        descriptor.height,
        descriptor.cfa.clone(),
        descriptor.black_levels,
        descriptor.white_levels,
        descriptor.decoded_samples_sha256.clone(),
        InferRawFoundationDecoderIdentity::new(
            descriptor.decoder_provider_id.clone(),
            descriptor.decoder_provider_version.clone(),
        ),
    )?;
    let request = InferRawFoundationRequest::new(
        InferRawFoundationPriority::Interactive,
        None,
        source_revision,
        InferRawFoundationSource::new(source_sha256, source_size_bytes),
        staging,
    );
    request.validate()?;
    Ok(request)
}

fn source_revision(source_sha256: &str, staging: &IsolatedRawFrameStaging) -> String {
    format!(
        "shadow:raw-foundation/source:{source_sha256}/staging:{}",
        staging.descriptor().decoded_samples_sha256
    )
}

fn alias_expectation<'value>(
    source_revision: &'value str,
    source_sha256: &'value str,
    source_size_bytes: u64,
    staging: &'value IsolatedRawFrameStaging,
) -> InferCacheAliasExpectation<'value> {
    InferCacheAliasExpectation {
        source_revision,
        source_sha256,
        source_size_bytes,
        decoded_samples_sha256: &staging.descriptor().decoded_samples_sha256,
        source_pixel_contract_sha256: RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
        model_package_sha256: RAWNIND_FOUNDATION_PACKAGE_SHA256,
        model_graph_sha256: RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256,
        implementation_revision: IMPLEMENTATION_REVISION,
    }
}

fn execute_with_cancellation(
    provider: &InferRuntimeClient,
    lease: shadow_ai::InferRawFoundationRegisteredLease,
    cancellation: &CancellationToken,
) -> Result<Option<InferRawFoundationResult>, InferMaterializationError> {
    let job = lease.job();
    thread::scope(|scope| {
        let worker = scope.spawn(move || provider.execute_raw_foundation(lease));
        let mut cancellation_sent = false;
        while !worker.is_finished() {
            if cancellation.is_cancelled() && !cancellation_sent {
                let _ = provider.cancel_raw_foundation(&job);
                cancellation_sent = true;
            }
            thread::sleep(EXECUTION_POLL_INTERVAL);
        }
        let result = worker
            .join()
            .map_err(|_| InferMaterializationError::ExecutionThreadPanicked)?;
        if cancellation.is_cancelled() {
            Ok(None)
        } else {
            result.map(Some).map_err(Into::into)
        }
    })
}

fn validate_result(
    result: &InferRawFoundationResult,
    verification: &FoundationArtifactVerification,
    expected: &InferCacheAliasExpectation<'_>,
) -> Result<(), InferMaterializationError> {
    let artifact = &result.artifact;
    if artifact.cache_key_sha256 != verification.cache_key_sha256
        || artifact.artifact_identity_sha256 != verification.artifact_identity_sha256
        || artifact.artifact_file_sha256 != verification.file_sha256
        || artifact.artifact_file_bytes != verification.file_bytes
        || artifact.sequence_sha256 != verification.sequence_sha256
        || artifact.output_width != verification.width
        || artifact.output_height != verification.height
        || artifact.implementation_revision != verification.implementation_revision
        || verification.source_sha256 != expected.source_sha256
        || verification.source_size_bytes != expected.source_size_bytes
        || verification.source_pixel_contract_sha256 != expected.source_pixel_contract_sha256
        || verification.model_package_sha256 != expected.model_package_sha256
        || verification.model_graph_sha256 != expected.model_graph_sha256
        || verification.implementation_revision != expected.implementation_revision
    {
        return Err(InferMaterializationError::ReceiptMismatch);
    }
    Ok(())
}

fn descriptor_from_verification(
    verification: &FoundationArtifactVerification,
) -> Result<RawFoundationArtifact, RawFoundationArtifactError> {
    RawFoundationArtifact::new(
        GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Sha256,
            verification.file_sha256.clone(),
            verification.file_bytes,
            RAW_FOUNDATION_MEDIA_TYPE.into(),
            RAW_FOUNDATION_ENCODING_VERSION,
        )?,
        RasterExtent::new(verification.width, verification.height)?,
        RawFoundationSourceProvenance::new(
            verification.source_sha256.clone(),
            verification.source_size_bytes,
            verification.source_pixel_contract_sha256.clone(),
        )?,
        RawFoundationProvenance::new(
            verification.cache_key_sha256.clone(),
            verification.artifact_identity_sha256.clone(),
            verification.model_package_sha256.clone(),
            verification.model_graph_sha256.clone(),
            verification.implementation_revision.clone(),
        )?,
    )
}

#[cfg(unix)]
fn create_owner_output(path: &Path) -> Result<File, InferMaterializationError> {
    use std::os::unix::fs::OpenOptionsExt as _;

    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(InferMaterializationError::Io)
}

#[cfg(not(unix))]
fn create_owner_output(path: &Path) -> Result<File, InferMaterializationError> {
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(InferMaterializationError::Io)
}

struct OwnedPartial {
    store: FoundationArtifactStore,
    path: PathBuf,
    discard_on_drop: bool,
}

impl OwnedPartial {
    fn new(store: FoundationArtifactStore, path: PathBuf) -> Self {
        Self {
            store,
            path,
            discard_on_drop: true,
        }
    }

    fn path(&self) -> &Path {
        &self.path
    }

    fn preserve(&mut self) {
        self.discard_on_drop = false;
    }
}

impl Drop for OwnedPartial {
    fn drop(&mut self) {
        if self.discard_on_drop {
            let _ = self.store.discard_partial(&self.path);
        }
    }
}

#[derive(Debug, Error)]
pub(crate) enum InferMaterializationError {
    #[error(transparent)]
    Client(#[from] InferRuntimeClientError),
    #[error(transparent)]
    Store(#[from] FoundationArtifactStoreError),
    #[error(transparent)]
    Alias(#[from] InferCacheAliasError),
    #[error(transparent)]
    Artifact(#[from] RawFoundationArtifactError),
    #[error("RAW foundation Infer artifact receipt differs from Shadow verification")]
    ReceiptMismatch,
    #[error("RAW foundation Infer execution worker panicked")]
    ExecutionThreadPanicked,
    #[error("RAW foundation Infer local file I/O failed: {0}")]
    Io(#[from] std::io::Error),
}

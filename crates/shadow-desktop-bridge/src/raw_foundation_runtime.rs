//! Desktop application adapter for an Infer Runtime RawNIND foundation.
//!
//! Shadow retains source inventory, isolated RAW decode, local cache lookup,
//! independent publication, and stale-result boundaries. Infer Runtime owns
//! model execution and its user-installed model assets. The resulting cache
//! path is transient application state and never becomes part of a Recipe.

pub(crate) mod config;
mod infer_materialization;

/// Exact execution implementation admitted by the configured Infer consumer.
/// Keep its provenance distinct from the native reference implementation.
pub(crate) const INFER_RAW_FOUNDATION_IMPLEMENTATION_REVISION: &str =
    "rawnind-public-bayer-foundation-ort127-exp1";

use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex, MutexGuard, TryLockError},
    thread,
    time::Duration,
};

use shadow_ai::{
    CancellationToken, RAWNIND_FOUNDATION_MODEL_ID, RawFoundationArtifact,
    RawFoundationMaterializationDisposition, RuntimeProgressSink,
};
use shadow_cache::{
    FoundationArtifactError, FoundationArtifactReader, FoundationArtifactStore,
    FoundationArtifactStoreError, sha256_file,
};
use shadow_catalog::RepresentationFingerprint;
use shadow_core::fingerprint_source;
use thiserror::Error;

use self::{
    config::RawFoundationRuntimePaths,
    infer_materialization::{
        InferMaterializationError, InferMaterializationOutcome, InferMaterializedFoundation,
        InferRawFoundationMaterializer,
    },
};
use crate::isolated_proxy::{
    IsolatedRawFrameStaging, configured_helper_path, stage_isolated_raw_frame,
};
use crate::raw_foundation_noise_assessment::{
    RawFoundationNoiseAssessment, RawFoundationNoiseAssessmentError, assess_staged_bayer_noise,
};

const SOURCE_PREPARATION_WAIT_INTERVAL: Duration = Duration::from_millis(10);

#[derive(Debug)]
pub(crate) struct RawFoundationRuntime {
    raw_frame_staging_root: PathBuf,
    store: FoundationArtifactStore,
    infer_materializer: InferRawFoundationMaterializer,
    /// One exact active-source staging is enough to bridge the automatic
    /// noise assessment and a following materialization. The staging owner
    /// itself caps the file-backed Bayer payload at 512 MiB.
    prepared_source: Mutex<PreparedRawFoundationSourceCache<IsolatedRawFrameStaging>>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
struct PreparedRawFoundationSourceIdentity {
    source_path: PathBuf,
    source: RepresentationFingerprint,
    source_sha256: String,
}

#[derive(Debug)]
struct PreparedRawFoundationSourceEntry<Payload> {
    identity: PreparedRawFoundationSourceIdentity,
    payload: Arc<Payload>,
}

#[derive(Debug)]
struct PreparedRawFoundationSourceCache<Payload> {
    entry: Option<PreparedRawFoundationSourceEntry<Payload>>,
}

impl<Payload> Default for PreparedRawFoundationSourceCache<Payload> {
    fn default() -> Self {
        Self { entry: None }
    }
}

impl<Payload> PreparedRawFoundationSourceCache<Payload> {
    fn get_or_prepare<Error>(
        &mut self,
        identity: PreparedRawFoundationSourceIdentity,
        prepare: impl FnOnce() -> Result<Payload, Error>,
    ) -> Result<Arc<Payload>, Error> {
        if let Some(entry) = self
            .entry
            .as_ref()
            .filter(|entry| entry.identity == identity)
        {
            return Ok(Arc::clone(&entry.payload));
        }
        let payload = Arc::new(prepare()?);
        self.entry = Some(PreparedRawFoundationSourceEntry { identity, payload });
        Ok(Arc::clone(
            &self
                .entry
                .as_ref()
                .expect("prepared source inserted")
                .payload,
        ))
    }
}

#[derive(Debug, Clone)]
struct PreparedRawFoundationSource {
    source: RepresentationFingerprint,
    source_sha256: String,
    staging: Arc<IsolatedRawFrameStaging>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationInvocation {
    pub(crate) request_id: String,
    pub(crate) generation: u64,
    pub(crate) photo_id: String,
    pub(crate) input_raw: PathBuf,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationRuntimeAvailability {
    pub(crate) model_id: String,
    pub(crate) runtime_version: String,
}

#[derive(Debug, Clone)]
pub(crate) struct RawFoundationReady {
    pub(crate) descriptor: RawFoundationArtifact,
    pub(crate) path: PathBuf,
    pub(crate) source_path: PathBuf,
    pub(crate) source: RepresentationFingerprint,
    pub(crate) disposition: RawFoundationMaterializationDisposition,
    pub(crate) verified_reader: Option<Arc<Mutex<FoundationArtifactReader>>>,
    pub(crate) raw_frame_staging: Option<Arc<IsolatedRawFrameStaging>>,
}

impl PartialEq for RawFoundationReady {
    fn eq(&self, other: &Self) -> bool {
        self.descriptor == other.descriptor
            && self.path == other.path
            && self.source_path == other.source_path
            && self.source == other.source
            && self.disposition == other.disposition
    }
}

impl Eq for RawFoundationReady {}

#[derive(Debug, Clone, PartialEq)]
pub(crate) enum RawFoundationRuntimeOutcome {
    Ready(Box<RawFoundationReady>),
    Unavailable { diagnostic: String },
    Cancelled,
}

impl RawFoundationRuntime {
    pub(crate) fn open(
        paths: RawFoundationRuntimePaths,
    ) -> Result<Self, RawFoundationRuntimeError> {
        let store = FoundationArtifactStore::open(&paths.foundation_store_root)?;
        let infer_materializer = InferRawFoundationMaterializer::new(
            paths.infer_base_url_override,
            paths.infer_credential_file,
            &paths.foundation_store_root,
        );
        Ok(Self {
            raw_frame_staging_root: paths.raw_frame_staging_root,
            store,
            infer_materializer,
            prepared_source: Mutex::new(PreparedRawFoundationSourceCache::default()),
        })
    }

    /// Verifies Infer Runtime reachability without exposing RAW bytes or paths.
    pub(crate) fn probe(
        &self,
        _cancellation: &CancellationToken,
    ) -> Result<RawFoundationRuntimeAvailability, RawFoundationRuntimeError> {
        self.infer_materializer.probe_client()?;
        Ok(RawFoundationRuntimeAvailability {
            model_id: RAWNIND_FOUNDATION_MODEL_ID.into(),
            runtime_version: "infer-runtime".into(),
        })
    }

    pub(crate) fn materialize(
        &self,
        invocation: &RawFoundationInvocation,
        cancellation: &CancellationToken,
        progress: &dyn RuntimeProgressSink,
    ) -> Result<RawFoundationRuntimeOutcome, RawFoundationRuntimeError> {
        if cancellation.is_cancelled() {
            return Ok(RawFoundationRuntimeOutcome::Cancelled);
        }
        let Some(prepared) = self.prepare_source(&invocation.input_raw, cancellation)? else {
            return Ok(RawFoundationRuntimeOutcome::Cancelled);
        };

        self.materialize_infer(invocation, prepared, cancellation, progress)
    }

    /// Estimates visible sensor noise from the isolated provider-neutral Bayer
    /// staging. This path never verifies, admits, or executes the AI model.
    pub(crate) fn assess_noise(
        &self,
        input_raw: &Path,
    ) -> Result<RawFoundationNoiseAssessment, RawFoundationRuntimeError> {
        let prepared = self
            .prepare_source(input_raw, &CancellationToken::default())?
            .ok_or(RawFoundationRuntimeError::Cancelled)?;
        Ok(assess_staged_bayer_noise(prepared.staging.manifest_path())?)
    }

    /// Recomputes the exact plan and resolves only an existing verified cache hit.
    ///
    /// This supports persistent Recipe intent after process restart without
    /// admitting Infer Runtime work or creating a cache partial.
    pub(crate) fn resolve_cached(
        &self,
        input_raw: &Path,
        cancellation: &CancellationToken,
    ) -> Result<Option<RawFoundationReady>, RawFoundationRuntimeError> {
        let prepared = self
            .prepare_source(input_raw, cancellation)?
            .ok_or(RawFoundationRuntimeError::Cancelled)?;
        let cached = self.infer_materializer.resolve_cached(
            &self.store,
            &prepared.source_sha256,
            prepared.source.byte_len,
            &prepared.staging,
        )?;
        if fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?
            != prepared.source
        {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        Ok(cached.map(|materialized| {
            ready_from_infer(materialized, input_raw, prepared.source, prepared.staging)
        }))
    }

    fn prepare_source(
        &self,
        input_raw: &Path,
        cancellation: &CancellationToken,
    ) -> Result<Option<PreparedRawFoundationSource>, RawFoundationRuntimeError> {
        let Some(mut cache) = self.prepared_source_guard(cancellation)? else {
            return Ok(None);
        };
        if cancellation.is_cancelled() {
            return Ok(None);
        }
        let before =
            fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?;
        let source_sha256 = sha256_file(input_raw)?;
        let after_hash =
            fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?;
        if before != after_hash {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        let identity = PreparedRawFoundationSourceIdentity {
            source_path: input_raw.to_path_buf(),
            source: before,
            source_sha256: source_sha256.clone(),
        };
        let staging = cache.get_or_prepare(identity, || self.stage_decoded_raw_frame(input_raw))?;
        if cancellation.is_cancelled() {
            return Ok(None);
        }
        if fingerprint_source(input_raw).map_err(RawFoundationRuntimeError::SourceInventory)?
            != before
        {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        Ok(Some(PreparedRawFoundationSource {
            source: before,
            source_sha256,
            staging,
        }))
    }

    fn prepared_source_guard(
        &self,
        cancellation: &CancellationToken,
    ) -> Result<
        Option<MutexGuard<'_, PreparedRawFoundationSourceCache<IsolatedRawFrameStaging>>>,
        RawFoundationRuntimeError,
    > {
        loop {
            if cancellation.is_cancelled() {
                return Ok(None);
            }
            match self.prepared_source.try_lock() {
                Ok(cache) => return Ok(Some(cache)),
                Err(TryLockError::WouldBlock) => thread::sleep(SOURCE_PREPARATION_WAIT_INTERVAL),
                Err(TryLockError::Poisoned(_)) => {
                    return Err(RawFoundationRuntimeError::SourcePreparationStatePoisoned);
                }
            }
        }
    }

    fn stage_decoded_raw_frame(
        &self,
        input_raw: &Path,
    ) -> Result<IsolatedRawFrameStaging, RawFoundationRuntimeError> {
        let helper_path = configured_helper_path().ok_or_else(|| {
            RawFoundationRuntimeError::DecodedInput(
                "isolated RAW decode helper is unavailable".to_owned(),
            )
        })?;
        stage_isolated_raw_frame(&helper_path, &self.raw_frame_staging_root, input_raw)
            .map_err(|error| RawFoundationRuntimeError::DecodedInput(error.to_string()))
    }

    fn materialize_infer(
        &self,
        invocation: &RawFoundationInvocation,
        prepared: PreparedRawFoundationSource,
        cancellation: &CancellationToken,
        progress: &dyn RuntimeProgressSink,
    ) -> Result<RawFoundationRuntimeOutcome, RawFoundationRuntimeError> {
        let outcome = match self.infer_materializer.materialize(
            &self.store,
            &prepared.source_sha256,
            prepared.source.byte_len,
            &prepared.staging,
            cancellation,
            progress,
        ) {
            Ok(outcome) => outcome,
            Err(InferMaterializationError::Client(error)) => {
                return Ok(RawFoundationRuntimeOutcome::Unavailable {
                    diagnostic: format!("Infer Runtime is unavailable: {error}"),
                });
            }
            Err(error) => return Err(error.into()),
        };
        if fingerprint_source(&invocation.input_raw)
            .map_err(RawFoundationRuntimeError::SourceInventory)?
            != prepared.source
        {
            return Err(RawFoundationRuntimeError::SourceChanged);
        }
        Ok(match outcome {
            InferMaterializationOutcome::Ready(materialized) => {
                RawFoundationRuntimeOutcome::Ready(Box::new(ready_from_infer(
                    *materialized,
                    &invocation.input_raw,
                    prepared.source,
                    prepared.staging,
                )))
            }
            InferMaterializationOutcome::Cancelled => RawFoundationRuntimeOutcome::Cancelled,
        })
    }
}

fn ready_from_infer(
    materialized: InferMaterializedFoundation,
    source_path: &Path,
    source: RepresentationFingerprint,
    raw_frame_staging: Arc<IsolatedRawFrameStaging>,
) -> RawFoundationReady {
    RawFoundationReady {
        descriptor: materialized.descriptor,
        path: materialized.path,
        source_path: source_path.to_path_buf(),
        source,
        disposition: materialized.disposition,
        verified_reader: materialized
            .verified_reader
            .map(|reader| Arc::new(Mutex::new(reader))),
        raw_frame_staging: Some(raw_frame_staging),
    }
}

#[derive(Debug, Error)]
pub(crate) enum RawFoundationRuntimeError {
    #[error("RAW foundation source could not be inventoried")]
    SourceInventory(#[source] std::io::Error),
    #[error("RAW foundation source changed during materialization")]
    SourceChanged,
    #[error("RAW foundation could not stage the decoded Bayer input: {0}")]
    DecodedInput(String),
    #[error("RAW foundation could not assess source noise: {0}")]
    NoiseAssessment(#[from] RawFoundationNoiseAssessmentError),
    #[error("RAW foundation cache resolution was cancelled")]
    Cancelled,
    #[error("RAW foundation source preparation state is poisoned")]
    SourcePreparationStatePoisoned,
    #[error(transparent)]
    SourceHash(#[from] FoundationArtifactError),
    #[error(transparent)]
    InferMaterialization(#[from] InferMaterializationError),
    #[error(transparent)]
    Store(#[from] FoundationArtifactStoreError),
}

#[cfg(test)]
mod tests;

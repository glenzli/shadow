//! Typed client for infer-runtime's inactive `RawNIND` foundation route.
//!
//! Shadow still owns RAW decoding, cache lookup, artifact verification,
//! publication, and stale-result arbitration. This owner begins only after a
//! cache miss. It pins the daemon endpoint selected for ticket creation,
//! passes already-open staging/output handles over the owner-only local lease
//! transport, and admits only the exact experimental Build currently frozen
//! by the cross-project contract.

use std::{fmt, fs::File, path::PathBuf, time::Duration};

use serde::{Deserialize, Serialize};

use super::{DiscoveryEndpoint, InferRuntimeClient, InferRuntimeClientError};

const CREATE_LEASE_PATH: &str = "infer/v1/raw/foundations/leases";
const EXECUTE_PATH: &str = "infer/v1/raw/foundations";
const RAW_FOUNDATION_INTENT: &str = "raw.materialize_foundation";
const SOURCE_PIXEL_CONTRACT_SHA256: &str =
    "e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f";
const STAGING_SCHEMA: &str = "infer.raw-foundation-staging@20260811.1";
const STAGING_SAMPLE_FORMAT: &str = "uint16-le-row-major-active-bayer";
const ARTIFACT_LEASE_CONTRACT: &str = "infer-runtime.artifact-lease@20260811.1";
const UNIX_ARTIFACT_LEASE_TRANSPORT: &str = "uds-scm-rights";
const MAX_SOURCE_BYTES: u64 = 2 * 1024 * 1024 * 1024;
const MAX_DIMENSION: u32 = 100_000;
const MAX_ID_BYTES: usize = 256;
const MAX_CAPABILITY_ID_BYTES: usize = 128;
const DEFAULT_EXECUTION_TIMEOUT: Duration = Duration::from_hours(1);
const EXECUTION_RESPONSE_GRACE: Duration = Duration::from_secs(5);

const EXPECTED_PROVIDER: &str = "raw-foundation-local";
const EXPECTED_DEPLOYMENT: &str = "rawnind_ort127_exp1";
const EXPECTED_MODEL_PROFILE: &str = "rawnind";
const EXPECTED_MODEL_BUILD: &str = "rawnind_ort127_exp1";
const EXPECTED_PHYSICAL_MODEL: &str = "darktable-ai/rawnind-public-bayer";
const EXPECTED_EXACT_REVISION: &str = "release-5.6.0@5454d7aa6d89a67054fd4a83343b09e69acaf76a";
const EXPECTED_GRAPH_SHA256: &str =
    "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901";
const EXPECTED_IMPLEMENTATION_REVISION: &str = "rawnind-public-bayer-foundation-ort127-exp1";
const EXPECTED_CACHE_IDENTITY: &str = "rawnind-cpu-ort127-exp1";
const EXPECTED_EXECUTION_PROVIDER: &str = "CPUExecutionProvider";
const EXPECTED_RUNTIME_VERSION: &str = "onnxruntime-1.27.0";
const EXPECTED_PRECISION: &str = "float32";

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum InferRawFoundationPriority {
    Interactive,
    Background,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct InferRawFoundationSource {
    pub sha256: String,
    pub size_bytes: u64,
    pub pixel_contract_sha256: String,
}

impl InferRawFoundationSource {
    /// Binds a source RAW identity to the only pixel contract currently
    /// accepted by the `RawNIND` foundation Build.
    pub fn new(sha256: impl Into<String>, size_bytes: u64) -> Self {
        Self {
            sha256: sha256.into(),
            size_bytes,
            pixel_contract_sha256: SOURCE_PIXEL_CONTRACT_SHA256.into(),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct InferRawFoundationStaging {
    pub schema: String,
    pub width: u32,
    pub height: u32,
    pub cfa: String,
    pub black_levels: [u16; 4],
    pub white_levels: [u16; 4],
    pub sample_format: String,
    pub sample_bytes: u64,
    pub decoded_samples_sha256: String,
    pub decoder_provider_id: String,
    pub decoder_provider_version: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct InferRawFoundationDecoderIdentity {
    pub provider_id: String,
    pub provider_version: String,
}

impl InferRawFoundationDecoderIdentity {
    pub fn new(provider_id: impl Into<String>, provider_version: impl Into<String>) -> Self {
        Self {
            provider_id: provider_id.into(),
            provider_version: provider_version.into(),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct InferRawFoundationRequest {
    model: &'static str,
    pub priority: InferRawFoundationPriority,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub deadline_ms: Option<u64>,
    pub source_revision: String,
    pub source: InferRawFoundationSource,
    pub staging: InferRawFoundationStaging,
}

impl InferRawFoundationRequest {
    pub fn new(
        priority: InferRawFoundationPriority,
        deadline_ms: Option<u64>,
        source_revision: impl Into<String>,
        source: InferRawFoundationSource,
        staging: InferRawFoundationStaging,
    ) -> Self {
        Self {
            model: RAW_FOUNDATION_INTENT,
            priority,
            deadline_ms,
            source_revision: source_revision.into(),
            source,
            staging,
        }
    }

    /// Validates the complete request before creating a remote Job or ticket.
    ///
    /// # Errors
    ///
    /// Rejects unsupported pixel/staging contracts, malformed identities, or
    /// dimensions and byte counts that cannot describe the staged Bayer file.
    pub fn validate(&self) -> Result<(), InferRuntimeClientError> {
        if matches!(self.deadline_ms, Some(0 | 3_600_001..)) {
            return invalid_request("deadline_ms must be in 1..=3600000");
        }
        validate_id(
            &self.source_revision,
            MAX_ID_BYTES,
            "source_revision is invalid",
        )?;
        validate_sha256(&self.source.sha256, "source SHA-256 is invalid")?;
        if self.source.size_bytes == 0 || self.source.size_bytes > MAX_SOURCE_BYTES {
            return invalid_request("source size is outside the supported bound");
        }
        if self.source.pixel_contract_sha256 != SOURCE_PIXEL_CONTRACT_SHA256 {
            return invalid_request("source pixel contract is unsupported");
        }
        self.staging.validate()
    }

    fn execution_timeout(&self) -> Duration {
        self.deadline_ms
            .map_or(DEFAULT_EXECUTION_TIMEOUT, Duration::from_millis)
            .saturating_add(EXECUTION_RESPONSE_GRACE)
    }
}

impl InferRawFoundationStaging {
    /// Constructs the frozen staging descriptor while keeping the model-owned
    /// constants out of application call sites.
    ///
    /// # Errors
    ///
    /// Rejects invalid dimensions, CFA/level contracts, decoded-sample
    /// identity, or decoder identity.
    pub fn new(
        width: u32,
        height: u32,
        cfa: impl Into<String>,
        black_levels: [u16; 4],
        white_levels: [u16; 4],
        decoded_samples_sha256: impl Into<String>,
        decoder: InferRawFoundationDecoderIdentity,
    ) -> Result<Self, InferRuntimeClientError> {
        let sample_bytes = u64::from(width)
            .checked_mul(u64::from(height))
            .and_then(|pixels| pixels.checked_mul(2))
            .ok_or(InferRuntimeClientError::InvalidRawFoundationRequest(
                "staging byte count overflows",
            ))?;
        let staging = Self {
            schema: STAGING_SCHEMA.into(),
            width,
            height,
            cfa: cfa.into(),
            black_levels,
            white_levels,
            sample_format: STAGING_SAMPLE_FORMAT.into(),
            sample_bytes,
            decoded_samples_sha256: decoded_samples_sha256.into(),
            decoder_provider_id: decoder.provider_id,
            decoder_provider_version: decoder.provider_version,
        };
        staging.validate()?;
        Ok(staging)
    }

    fn validate(&self) -> Result<(), InferRuntimeClientError> {
        if self.schema != STAGING_SCHEMA {
            return invalid_request("staging schema is unsupported");
        }
        if self.width < 4
            || self.height < 4
            || self.width > MAX_DIMENSION
            || self.height > MAX_DIMENSION
        {
            return invalid_request("staging dimensions are outside the supported bound");
        }
        if self.cfa.len() != 4 {
            return invalid_request("staging CFA must describe one Bayer cell");
        }
        let mut sites = self.cfa.as_bytes().to_vec();
        sites.sort_unstable();
        if sites != b"BGGR" {
            return invalid_request("staging CFA must contain one R, two G, and one B site");
        }
        if self
            .black_levels
            .iter()
            .zip(self.white_levels)
            .any(|(black, white)| *black >= white)
        {
            return invalid_request("every black level must be below its white level");
        }
        if self
            .white_levels
            .iter()
            .any(|white| *white != self.white_levels[0])
        {
            return invalid_request("RawNIND requires one shared white level");
        }
        if self.sample_format != STAGING_SAMPLE_FORMAT {
            return invalid_request("staging sample format is unsupported");
        }
        let expected_bytes = u64::from(self.width)
            .checked_mul(u64::from(self.height))
            .and_then(|pixels| pixels.checked_mul(2))
            .ok_or(InferRuntimeClientError::InvalidRawFoundationRequest(
                "staging byte count overflows",
            ))?;
        if self.sample_bytes != expected_bytes {
            return invalid_request("staging byte count does not match its dimensions");
        }
        validate_sha256(
            &self.decoded_samples_sha256,
            "decoded sample SHA-256 is invalid",
        )?;
        validate_id(
            &self.decoder_provider_id,
            MAX_ID_BYTES,
            "decoder provider id is invalid",
        )?;
        validate_id(
            &self.decoder_provider_version,
            MAX_ID_BYTES,
            "decoder provider version is invalid",
        )
    }
}

#[derive(Clone)]
pub struct InferRawFoundationJob {
    id: String,
    endpoint: DiscoveryEndpoint,
}

impl fmt::Debug for InferRawFoundationJob {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InferRawFoundationJob")
            .field("id", &self.id)
            .finish_non_exhaustive()
    }
}

impl InferRawFoundationJob {
    pub fn id(&self) -> &str {
        &self.id
    }
}

pub struct InferRawFoundationLeaseGrant {
    job: InferRawFoundationJob,
    ticket_id: String,
    expires_at_unix_ms: u64,
    daemon_generation: String,
    socket_path: PathBuf,
    source_revision: String,
    width: u32,
    height: u32,
    execution_timeout: Duration,
}

impl fmt::Debug for InferRawFoundationLeaseGrant {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InferRawFoundationLeaseGrant")
            .field("job_id", &self.job.id)
            .field("ticket_id", &"<redacted>")
            .field("expires_at_unix_ms", &self.expires_at_unix_ms)
            .field("daemon_generation", &self.daemon_generation)
            .field("socket_path", &"<redacted-owner-only-endpoint>")
            .finish_non_exhaustive()
    }
}

impl InferRawFoundationLeaseGrant {
    pub fn job(&self) -> InferRawFoundationJob {
        self.job.clone()
    }

    pub const fn expires_at_unix_ms(&self) -> u64 {
        self.expires_at_unix_ms
    }

    pub fn daemon_generation(&self) -> &str {
        &self.daemon_generation
    }
}

pub struct InferRawFoundationRegisteredLease {
    job: InferRawFoundationJob,
    lease_id: String,
    expires_at_unix_ms: u64,
    source_revision: String,
    width: u32,
    height: u32,
    execution_timeout: Duration,
    output: File,
}

impl fmt::Debug for InferRawFoundationRegisteredLease {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InferRawFoundationRegisteredLease")
            .field("job_id", &self.job.id)
            .field("lease_id", &"<redacted>")
            .field("expires_at_unix_ms", &self.expires_at_unix_ms)
            .finish_non_exhaustive()
    }
}

impl InferRawFoundationRegisteredLease {
    pub fn job(&self) -> InferRawFoundationJob {
        self.job.clone()
    }

    pub const fn expires_at_unix_ms(&self) -> u64 {
        self.expires_at_unix_ms
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Deserialize)]
pub struct InferRawFoundationArtifactReceipt {
    pub cache_key_sha256: String,
    pub artifact_identity_sha256: String,
    pub artifact_file_sha256: String,
    pub artifact_file_bytes: u64,
    pub sequence_sha256: String,
    pub payload_sha256: String,
    pub output_width: u32,
    pub output_height: u32,
    pub tile_inferences: u64,
    pub maximum_accumulator_rows: u32,
    pub explicit_full_output_buffers: u32,
    pub implementation_revision: String,
    pub cache_identity: String,
}

#[derive(Debug, Clone, Eq, PartialEq, Deserialize)]
pub struct InferRawFoundationProvenance {
    pub provider: String,
    pub deployment: String,
    pub model_profile: String,
    pub model_build: String,
    pub physical_model: String,
    pub exact_revision: String,
    pub graph_sha256: String,
    pub implementation_revision: String,
    pub cache_identity: String,
    pub execution_provider: String,
    pub runtime_version: String,
    pub precision: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct InferRawFoundationResult {
    pub job_id: String,
    pub source_revision: String,
    pub artifact: InferRawFoundationArtifactReceipt,
    pub provenance: InferRawFoundationProvenance,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct InferRawFoundationCancellation {
    pub job_id: String,
}

pub trait InferRawFoundationProvider {
    /// Creates an authenticated Job and a short-lived, one-shot handle ticket.
    ///
    /// # Errors
    ///
    /// Returns a request, authentication, admission, transport, or typed
    /// response failure without exposing the ticket capability.
    fn begin_raw_foundation(
        &self,
        request: &InferRawFoundationRequest,
    ) -> Result<InferRawFoundationLeaseGrant, InferRuntimeClientError>;

    /// Sends the already-open read-only Bayer input and empty writable output
    /// handles over the lease's owner-only local transport.
    ///
    /// # Errors
    ///
    /// Returns a handle, endpoint-ownership, peer, framing, or lease protocol
    /// failure without falling back to a path or byte upload.
    fn register_raw_foundation_handles(
        &self,
        grant: InferRawFoundationLeaseGrant,
        input: &File,
        output: &File,
    ) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError>;

    /// Executes one registered lease against the exact daemon which issued it.
    ///
    /// # Errors
    ///
    /// Returns a local HTTP, Runtime, typed response, or output-handle length
    /// failure. It never rediscovers another daemon for a bound lease.
    fn execute_raw_foundation(
        &self,
        lease: InferRawFoundationRegisteredLease,
    ) -> Result<InferRawFoundationResult, InferRuntimeClientError>;

    /// Cancels a pending RAW Job and revokes its unconsumed ticket or lease.
    ///
    /// # Errors
    ///
    /// Returns a local HTTP or typed cancellation response failure.
    fn cancel_raw_foundation(
        &self,
        job: &InferRawFoundationJob,
    ) -> Result<InferRawFoundationCancellation, InferRuntimeClientError>;

    /// Runs the three-step experimental protocol. Cache lookup must already
    /// have completed in Shadow before this convenience method is called.
    ///
    /// # Errors
    ///
    /// Returns the first request, lease-registration, or execution failure.
    /// Registration failure also triggers a best-effort Job cancellation.
    fn materialize_raw_foundation(
        &self,
        request: &InferRawFoundationRequest,
        input: &File,
        output: &File,
    ) -> Result<InferRawFoundationResult, InferRuntimeClientError> {
        let grant = self.begin_raw_foundation(request)?;
        let job = grant.job();
        let lease = match self.register_raw_foundation_handles(grant, input, output) {
            Ok(lease) => lease,
            Err(error) => {
                let _ = self.cancel_raw_foundation(&job);
                return Err(error);
            }
        };
        self.execute_raw_foundation(lease)
    }
}

impl InferRawFoundationProvider for InferRuntimeClient {
    fn begin_raw_foundation(
        &self,
        request: &InferRawFoundationRequest,
    ) -> Result<InferRawFoundationLeaseGrant, InferRuntimeClientError> {
        request.validate()?;
        let (response, endpoint): (RawLeaseGrantResponse, DiscoveryEndpoint) = self
            .send_json_with_endpoint(CREATE_LEASE_PATH, |url, _consumer_version| {
                Ok(self
                    .client
                    .post(url)
                    .bearer_auth(self.credential.expose())
                    .json(request))
            })?;
        response.validate(request, endpoint)
    }

    fn register_raw_foundation_handles(
        &self,
        grant: InferRawFoundationLeaseGrant,
        input: &File,
        output: &File,
    ) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError> {
        register_handles(grant, input, output)
    }

    fn execute_raw_foundation(
        &self,
        lease: InferRawFoundationRegisteredLease,
    ) -> Result<InferRawFoundationResult, InferRuntimeClientError> {
        let request = RawExecuteRequest {
            job_id: &lease.job.id,
            lease_id: &lease.lease_id,
        };
        let response: RawFoundationResponse =
            Self::send_json_at(&lease.job.endpoint, EXECUTE_PATH, |url| {
                Ok(self
                    .client
                    .post(url)
                    .timeout(lease.execution_timeout)
                    .bearer_auth(self.credential.expose())
                    .json(&request))
            })?;
        let result = response.validate(
            &lease.job.id,
            &lease.source_revision,
            lease.width,
            lease.height,
        )?;
        let output_bytes = lease
            .output
            .metadata()
            .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?
            .len();
        if output_bytes != result.artifact.artifact_file_bytes {
            return Err(InferRuntimeClientError::InvalidResponse(
                "RAW output handle length does not match the artifact receipt",
            ));
        }
        Ok(result)
    }

    fn cancel_raw_foundation(
        &self,
        job: &InferRawFoundationJob,
    ) -> Result<InferRawFoundationCancellation, InferRuntimeClientError> {
        validate_capability_id(&job.id, "RAW job id is invalid")?;
        let path = format!("infer/v1/raw/foundations/{}/cancel", job.id);
        let response: RawCancellationResponse = Self::send_json_at(&job.endpoint, &path, |url| {
            Ok(self.client.post(url).bearer_auth(self.credential.expose()))
        })?;
        response.validate(&job.id)
    }
}

#[derive(Deserialize)]
struct RawLeaseGrantResponse {
    object: String,
    job_id: String,
    ticket_id: String,
    expires_at_unix_ms: u64,
    daemon_generation: String,
    binding: RawLeaseBinding,
}

#[derive(Deserialize)]
struct RawLeaseBinding {
    contract: String,
    transport: String,
    endpoint: String,
}

impl RawLeaseGrantResponse {
    fn validate(
        self,
        request: &InferRawFoundationRequest,
        endpoint: DiscoveryEndpoint,
    ) -> Result<InferRawFoundationLeaseGrant, InferRuntimeClientError> {
        if self.object != "raw.foundation.lease"
            || self.expires_at_unix_ms == 0
            || self.binding.contract != ARTIFACT_LEASE_CONTRACT
            || self.binding.transport != UNIX_ARTIFACT_LEASE_TRANSPORT
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "RAW lease grant violated the typed contract",
            ));
        }
        validate_capability_id(&self.job_id, "RAW job id is invalid")?;
        validate_capability_id(&self.ticket_id, "RAW ticket id is invalid")?;
        validate_capability_id(&self.daemon_generation, "RAW daemon generation is invalid")?;
        if self.binding.endpoint.is_empty() || self.binding.endpoint.len() > 1_024 {
            return Err(InferRuntimeClientError::InvalidResponse(
                "RAW lease endpoint violated the typed contract",
            ));
        }
        Ok(InferRawFoundationLeaseGrant {
            job: InferRawFoundationJob {
                id: self.job_id,
                endpoint,
            },
            ticket_id: self.ticket_id,
            expires_at_unix_ms: self.expires_at_unix_ms,
            daemon_generation: self.daemon_generation,
            socket_path: PathBuf::from(self.binding.endpoint),
            source_revision: request.source_revision.clone(),
            width: request.staging.width,
            height: request.staging.height,
            execution_timeout: request.execution_timeout(),
        })
    }
}

#[derive(Serialize)]
struct RawExecuteRequest<'a> {
    job_id: &'a str,
    lease_id: &'a str,
}

#[derive(Deserialize)]
struct RawFoundationResponse {
    id: String,
    object: String,
    status: String,
    source_revision: String,
    artifact: InferRawFoundationArtifactReceipt,
    provenance: InferRawFoundationProvenance,
}

impl RawFoundationResponse {
    fn validate(
        self,
        job_id: &str,
        source_revision: &str,
        width: u32,
        height: u32,
    ) -> Result<InferRawFoundationResult, InferRuntimeClientError> {
        if self.id != job_id
            || self.object != "raw.foundation"
            || self.status != "completed"
            || self.source_revision != source_revision
            || !self.artifact.is_valid(width, height)
            || !self.provenance.is_valid()
            || self.artifact.implementation_revision != self.provenance.implementation_revision
            || self.artifact.cache_identity != self.provenance.cache_identity
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "RAW foundation response violated the typed contract",
            ));
        }
        Ok(InferRawFoundationResult {
            job_id: self.id,
            source_revision: self.source_revision,
            artifact: self.artifact,
            provenance: self.provenance,
        })
    }
}

impl InferRawFoundationArtifactReceipt {
    fn is_valid(&self, width: u32, height: u32) -> bool {
        [
            self.cache_key_sha256.as_str(),
            self.artifact_identity_sha256.as_str(),
            self.artifact_file_sha256.as_str(),
            self.sequence_sha256.as_str(),
            self.payload_sha256.as_str(),
        ]
        .into_iter()
        .all(valid_sha256)
            && self.artifact_file_bytes > 0
            && self.output_width == width
            && self.output_height == height
            && self.tile_inferences > 0
            && self.maximum_accumulator_rows > 0
            && self.maximum_accumulator_rows <= height
            && self.explicit_full_output_buffers == 0
            && self.implementation_revision == EXPECTED_IMPLEMENTATION_REVISION
            && self.cache_identity == EXPECTED_CACHE_IDENTITY
    }
}

impl InferRawFoundationProvenance {
    fn is_valid(&self) -> bool {
        self.provider == EXPECTED_PROVIDER
            && self.deployment == EXPECTED_DEPLOYMENT
            && self.model_profile == EXPECTED_MODEL_PROFILE
            && self.model_build == EXPECTED_MODEL_BUILD
            && self.physical_model == EXPECTED_PHYSICAL_MODEL
            && self.exact_revision == EXPECTED_EXACT_REVISION
            && self.graph_sha256 == EXPECTED_GRAPH_SHA256
            && self.implementation_revision == EXPECTED_IMPLEMENTATION_REVISION
            && self.cache_identity == EXPECTED_CACHE_IDENTITY
            && self.execution_provider == EXPECTED_EXECUTION_PROVIDER
            && self.runtime_version == EXPECTED_RUNTIME_VERSION
            && self.precision == EXPECTED_PRECISION
    }
}

#[derive(Deserialize)]
struct RawCancellationResponse {
    id: String,
    object: String,
    status: String,
}

impl RawCancellationResponse {
    fn validate(
        self,
        expected_job_id: &str,
    ) -> Result<InferRawFoundationCancellation, InferRuntimeClientError> {
        if self.id != expected_job_id
            || self.object != "raw.foundation.cancellation"
            || self.status != "cancelled"
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "RAW cancellation response violated the typed contract",
            ));
        }
        Ok(InferRawFoundationCancellation { job_id: self.id })
    }
}

fn validate_id(
    value: &str,
    max_bytes: usize,
    error: &'static str,
) -> Result<(), InferRuntimeClientError> {
    if value.trim().is_empty() || value.len() > max_bytes || value.chars().any(char::is_control) {
        return invalid_request(error);
    }
    Ok(())
}

fn validate_sha256(value: &str, error: &'static str) -> Result<(), InferRuntimeClientError> {
    if !valid_sha256(value) {
        return invalid_request(error);
    }
    Ok(())
}

fn validate_capability_id(value: &str, error: &'static str) -> Result<(), InferRuntimeClientError> {
    if value.is_empty()
        || value.len() > MAX_CAPABILITY_ID_BYTES
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
    {
        return invalid_request(error);
    }
    Ok(())
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn invalid_request<T>(message: &'static str) -> Result<T, InferRuntimeClientError> {
    Err(InferRuntimeClientError::InvalidRawFoundationRequest(
        message,
    ))
}

#[cfg(unix)]
fn register_handles(
    grant: InferRawFoundationLeaseGrant,
    input: &File,
    output: &File,
) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError> {
    artifact_lease::register(grant, input, output)
}

#[cfg(not(unix))]
fn register_handles(
    _grant: InferRawFoundationLeaseGrant,
    _input: &File,
    _output: &File,
) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError> {
    Err(InferRuntimeClientError::RawArtifactLeaseUnsupported)
}

#[cfg(unix)]
mod artifact_lease;

#[cfg(test)]
mod tests;

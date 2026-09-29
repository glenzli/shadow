//! Typed Shadow boundary for Infer Runtime RAW foundation materialization.
//!
//! Shadow preserves the request/cache identities and sends only already-open
//! staging descriptors through the official lease and `SCM_RIGHTS` SDK. The
//! Runtime owns model execution; no generic HTTP RAW transport, file path, or
//! pixel payload is introduced here.

use std::{fmt, fs::File};

use serde::{Deserialize, Serialize};

use super::{InferRuntimeClient, InferRuntimeClientError};

const RAW_FOUNDATION_INTENT: &str = "raw.materialize_foundation";
pub const RAWNIND_FOUNDATION_MODEL_ID: &str = "darktable-ai/rawnind-public-bayer";
pub const RAWNIND_FOUNDATION_PACKAGE_SHA256: &str =
    "d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af";
pub const RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256: &str =
    "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901";
pub const RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256: &str =
    "e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f";
const STAGING_SCHEMA: &str = "infer.raw-foundation-staging@20260811.1";
const STAGING_SAMPLE_FORMAT: &str = "uint16-le-row-major-active-bayer";
const MAX_SOURCE_BYTES: u64 = 2 * 1024 * 1024 * 1024;
const MAX_DIMENSION: u32 = 100_000;
const MAX_ID_BYTES: usize = 256;

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
    pub fn new(sha256: impl Into<String>, size_bytes: u64) -> Self {
        Self {
            sha256: sha256.into(),
            size_bytes,
            pixel_contract_sha256: RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256.into(),
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

    /// Validates the cache/staging identity before any Runtime selection.
    ///
    /// # Errors
    ///
    /// Rejects malformed identities, dimensions, sizes, or pixel contracts.
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
        if self.source.pixel_contract_sha256 != RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256 {
            return invalid_request("source pixel contract is unsupported");
        }
        self.staging.validate()
    }
}

impl InferRawFoundationStaging {
    /// Constructs the frozen cache/staging identity without activating Infer.
    ///
    /// # Errors
    ///
    /// Rejects invalid dimensions, Bayer layout, levels, hashes, or decoder identity.
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
            .ok_or_else(|| raw_input("staging byte count overflows"))?;
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
            .ok_or_else(|| raw_input("staging byte count overflows"))?;
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
    binding: infer_runtime_client::RawFoundationLeaseBinding,
    expires_at_unix_ms: u64,
    daemon_generation: String,
}

impl fmt::Debug for InferRawFoundationLeaseGrant {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InferRawFoundationLeaseGrant")
            .field("job_id", &self.job.id)
            .field("expires_at_unix_ms", &self.expires_at_unix_ms)
            .field("daemon_generation", &self.daemon_generation)
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
}

impl fmt::Debug for InferRawFoundationRegisteredLease {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InferRawFoundationRegisteredLease")
            .field("job_id", &self.job.id)
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
    /// Begins the typed RAW protocol without placing a path or pixels on HTTP.
    ///
    /// # Errors
    ///
    /// Returns request validation, discovery, contract, or transport failures.
    fn begin_raw_foundation(
        &self,
        request: &InferRawFoundationRequest,
    ) -> Result<InferRawFoundationLeaseGrant, InferRuntimeClientError>;

    /// Registers RAW handles for an already issued typed lease.
    ///
    /// # Errors
    ///
    /// Returns handle-lease registration failures.
    fn register_raw_foundation_handles(
        &self,
        grant: InferRawFoundationLeaseGrant,
        input: &File,
        output: &File,
    ) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError>;

    /// Executes one typed RAW lease.
    ///
    /// # Errors
    ///
    /// Returns Runtime execution failures.
    fn execute_raw_foundation(
        &self,
        lease: InferRawFoundationRegisteredLease,
    ) -> Result<InferRawFoundationResult, InferRuntimeClientError>;

    /// Cancels one typed RAW Job.
    ///
    /// # Errors
    ///
    /// Returns Runtime cancellation failures.
    fn cancel_raw_foundation(
        &self,
        job: &InferRawFoundationJob,
    ) -> Result<InferRawFoundationCancellation, InferRuntimeClientError>;

    /// Runs the three-step protocol after Shadow has completed cache lookup.
    ///
    /// # Errors
    ///
    /// Returns request validation or typed Runtime failures.
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

/// Confirms that the pinned official SDK owns the typed RAW control and
/// descriptor-transfer surface. Availability remains a live Runtime concern.
///
/// # Errors
///
/// This static compatibility check currently cannot fail.
pub fn infer_raw_foundation_sdk_status() -> Result<(), InferRuntimeClientError> {
    Ok(())
}

impl InferRawFoundationProvider for InferRuntimeClient {
    fn begin_raw_foundation(
        &self,
        request: &InferRawFoundationRequest,
    ) -> Result<InferRawFoundationLeaseGrant, InferRuntimeClientError> {
        request.validate()?;
        let grant = self.block_on(self.sdk().create_raw_foundation_lease(
            &infer_runtime_client::RawFoundationLeaseRequest {
                model: RAW_FOUNDATION_INTENT.into(),
                priority: match request.priority {
                    InferRawFoundationPriority::Interactive => {
                        infer_runtime_client::RawFoundationPriority::Interactive
                    }
                    InferRawFoundationPriority::Background => {
                        infer_runtime_client::RawFoundationPriority::Background
                    }
                },
                deadline_ms: request.deadline_ms,
                source_revision: request.source_revision.clone(),
                source: infer_runtime_client::RawFoundationSource {
                    sha256: request.source.sha256.clone(),
                    size_bytes: request.source.size_bytes,
                    pixel_contract_sha256: request.source.pixel_contract_sha256.clone(),
                },
                staging: infer_runtime_client::RawFoundationStagingDescriptor {
                    schema: request.staging.schema.clone(),
                    width: request.staging.width,
                    height: request.staging.height,
                    cfa: request.staging.cfa.clone(),
                    black_levels: request.staging.black_levels,
                    white_levels: request.staging.white_levels,
                    sample_format: request.staging.sample_format.clone(),
                    sample_bytes: request.staging.sample_bytes,
                    decoded_samples_sha256: request.staging.decoded_samples_sha256.clone(),
                    decoder_provider_id: request.staging.decoder_provider_id.clone(),
                    decoder_provider_version: request.staging.decoder_provider_version.clone(),
                },
            },
        ))?;
        Ok(InferRawFoundationLeaseGrant {
            job: InferRawFoundationJob { id: grant.job_id },
            ticket_id: grant.ticket_id,
            binding: grant.binding,
            expires_at_unix_ms: grant.expires_at_unix_ms,
            daemon_generation: grant.daemon_generation,
        })
    }

    fn register_raw_foundation_handles(
        &self,
        grant: InferRawFoundationLeaseGrant,
        input: &File,
        output: &File,
    ) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError> {
        let lease_id = self.sdk().register_raw_foundation_handles(
            &infer_runtime_client::RawFoundationLeaseGrant {
                object: "raw.foundation_lease".into(),
                job_id: grant.job.id.clone(),
                ticket_id: grant.ticket_id,
                expires_at_unix_ms: grant.expires_at_unix_ms,
                daemon_generation: grant.daemon_generation,
                binding: grant.binding,
            },
            input,
            output,
        )?;
        Ok(InferRawFoundationRegisteredLease {
            job: grant.job,
            lease_id,
            expires_at_unix_ms: grant.expires_at_unix_ms,
        })
    }

    fn execute_raw_foundation(
        &self,
        lease: InferRawFoundationRegisteredLease,
    ) -> Result<InferRawFoundationResult, InferRuntimeClientError> {
        let response = self.block_on(
            self.sdk()
                .execute_raw_foundation(lease.job.id(), &lease.lease_id),
        )?;
        let artifact = response.artifact;
        Ok(InferRawFoundationResult {
            job_id: response.id,
            source_revision: response.source_revision,
            artifact: InferRawFoundationArtifactReceipt {
                cache_key_sha256: artifact.cache_key_sha256,
                artifact_identity_sha256: artifact.artifact_identity_sha256,
                artifact_file_sha256: artifact.artifact_file_sha256,
                artifact_file_bytes: artifact.artifact_file_bytes,
                sequence_sha256: artifact.sequence_sha256,
                payload_sha256: artifact.payload_sha256,
                output_width: u32::try_from(artifact.output_width)
                    .map_err(|_| raw_input("RAW output width exceeds Shadow's bound"))?,
                output_height: u32::try_from(artifact.output_height)
                    .map_err(|_| raw_input("RAW output height exceeds Shadow's bound"))?,
                tile_inferences: u64::try_from(artifact.tile_inferences)
                    .map_err(|_| raw_input("RAW tile count exceeds Shadow's bound"))?,
                maximum_accumulator_rows: u32::try_from(artifact.maximum_accumulator_rows)
                    .map_err(|_| raw_input("RAW accumulator rows exceed Shadow's bound"))?,
                explicit_full_output_buffers: u32::try_from(artifact.explicit_full_output_buffers)
                    .map_err(|_| raw_input("RAW output buffer count exceeds Shadow's bound"))?,
                implementation_revision: artifact.implementation_revision,
                cache_identity: artifact.cache_identity,
            },
            provenance: InferRawFoundationProvenance {
                provider: response.provenance.provider,
                deployment: response.provenance.deployment,
                model_profile: response.provenance.model_profile,
                model_build: response.provenance.model_build,
                physical_model: response.provenance.physical_model,
                exact_revision: response.provenance.exact_revision,
                graph_sha256: response.provenance.graph_sha256,
                implementation_revision: response.provenance.implementation_revision,
                cache_identity: response.provenance.cache_identity,
                execution_provider: response.provenance.execution_provider,
                runtime_version: response.provenance.runtime_version,
                precision: response.provenance.precision,
            },
        })
    }

    fn cancel_raw_foundation(
        &self,
        job: &InferRawFoundationJob,
    ) -> Result<InferRawFoundationCancellation, InferRuntimeClientError> {
        let response = self.block_on(self.sdk().cancel_raw_foundation(job.id()))?;
        Ok(InferRawFoundationCancellation {
            job_id: response.id,
        })
    }
}

fn raw_input(message: &str) -> InferRuntimeClientError {
    InferRuntimeClientError::Input(format!("Infer Runtime RAW request is invalid: {message}"))
}

fn invalid_request<T>(message: &str) -> Result<T, InferRuntimeClientError> {
    Err(raw_input(message))
}

fn validate_sha256(value: &str, message: &str) -> Result<(), InferRuntimeClientError> {
    if value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        Ok(())
    } else {
        invalid_request(message)
    }
}

fn validate_id(value: &str, maximum: usize, message: &str) -> Result<(), InferRuntimeClientError> {
    if !value.is_empty()
        && value.len() <= maximum
        && !value.chars().any(char::is_control)
        && value == value.trim()
    {
        Ok(())
    } else {
        invalid_request(message)
    }
}

#[cfg(test)]
mod tests;

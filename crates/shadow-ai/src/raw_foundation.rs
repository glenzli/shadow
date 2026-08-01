//! Portable AI output contract for a materialized RAW foundation.
//!
//! A RAW foundation is deliberately not an ordinary denoised raster. It
//! crosses from the sensor mosaic into full-resolution scene-linear camera
//! RGB and has its own cache, persistence, and rebuild lifecycle. Keeping this
//! contract separate preserves the same-domain invariant of
//! [`crate::DenoisedRasterArtifact`].

use serde::{Deserialize, Serialize};
use shadow_domain::ImageDomain;
use thiserror::Error;

use crate::{
    AiArtifactContractError, ArtifactHashAlgorithm, GeneratedArtifactReference, RasterExtent,
    RasterPixelLayout, RasterSampleFormat,
};

pub const RAW_FOUNDATION_MEDIA_TYPE: &str = "application/x-shadow-raw-foundation";
pub const RAW_FOUNDATION_ENCODING_VERSION: u32 = 1;
pub const MAX_RAW_FOUNDATION_IMPLEMENTATION_REVISION_BYTES: usize = 256;

/// Identity of the exact RAW source and decoded sensor contract.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RawFoundationSourceProvenance {
    #[serde(rename = "source_file_sha256")]
    file_sha256: String,
    #[serde(rename = "source_size_bytes")]
    size_bytes: u64,
    #[serde(rename = "source_pixel_contract_sha256")]
    pixel_contract_sha256: String,
}

impl RawFoundationSourceProvenance {
    /// Creates verified source provenance without retaining a local file path.
    ///
    /// # Errors
    ///
    /// Returns an error when either digest is not canonical lowercase SHA-256
    /// or when the source file is empty.
    pub fn new(
        source_file_sha256: String,
        source_size_bytes: u64,
        source_pixel_contract_sha256: String,
    ) -> Result<Self, RawFoundationArtifactError> {
        let provenance = Self {
            file_sha256: source_file_sha256,
            size_bytes: source_size_bytes,
            pixel_contract_sha256: source_pixel_contract_sha256,
        };
        provenance.validate()?;
        Ok(provenance)
    }

    pub fn source_file_sha256(&self) -> &str {
        &self.file_sha256
    }

    pub const fn source_size_bytes(&self) -> u64 {
        self.size_bytes
    }

    pub fn source_pixel_contract_sha256(&self) -> &str {
        &self.pixel_contract_sha256
    }

    /// Validates stable source identity without consulting the filesystem.
    ///
    /// # Errors
    ///
    /// Returns an error for malformed digests or an empty source.
    pub fn validate(&self) -> Result<(), RawFoundationArtifactError> {
        validate_sha256(&self.file_sha256, RawFoundationDigestField::SourceFile)?;
        if self.size_bytes == 0 {
            return Err(RawFoundationArtifactError::EmptySourceFile);
        }
        validate_sha256(
            &self.pixel_contract_sha256,
            RawFoundationDigestField::SourcePixelContract,
        )
    }
}

/// Rebuild and cache identity carried with a RAW foundation.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RawFoundationProvenance {
    cache_key_sha256: String,
    artifact_identity_sha256: String,
    model_package_sha256: String,
    model_graph_sha256: String,
    implementation_revision: String,
}

impl RawFoundationProvenance {
    /// Creates provenance for the exact model, implementation, and cache key.
    ///
    /// # Errors
    ///
    /// Returns an error for a non-canonical digest or an invalid
    /// implementation revision.
    pub fn new(
        cache_key_sha256: String,
        artifact_identity_sha256: String,
        model_package_sha256: String,
        model_graph_sha256: String,
        implementation_revision: String,
    ) -> Result<Self, RawFoundationArtifactError> {
        let provenance = Self {
            cache_key_sha256,
            artifact_identity_sha256,
            model_package_sha256,
            model_graph_sha256,
            implementation_revision,
        };
        provenance.validate()?;
        Ok(provenance)
    }

    pub fn cache_key_sha256(&self) -> &str {
        &self.cache_key_sha256
    }

    pub fn artifact_identity_sha256(&self) -> &str {
        &self.artifact_identity_sha256
    }

    pub fn model_package_sha256(&self) -> &str {
        &self.model_package_sha256
    }

    pub fn model_graph_sha256(&self) -> &str {
        &self.model_graph_sha256
    }

    pub fn implementation_revision(&self) -> &str {
        &self.implementation_revision
    }

    /// Validates all identities needed to reject stale or substituted output.
    ///
    /// # Errors
    ///
    /// Returns an error for a non-canonical digest or an invalid
    /// implementation revision.
    pub fn validate(&self) -> Result<(), RawFoundationArtifactError> {
        validate_sha256(&self.cache_key_sha256, RawFoundationDigestField::CacheKey)?;
        validate_sha256(
            &self.artifact_identity_sha256,
            RawFoundationDigestField::ArtifactIdentity,
        )?;
        validate_sha256(
            &self.model_package_sha256,
            RawFoundationDigestField::ModelPackage,
        )?;
        validate_sha256(
            &self.model_graph_sha256,
            RawFoundationDigestField::ModelGraph,
        )?;
        let revision = self.implementation_revision.trim();
        if revision.is_empty() || revision.len() > MAX_RAW_FOUNDATION_IMPLEMENTATION_REVISION_BYTES
        {
            return Err(RawFoundationArtifactError::InvalidImplementationRevision);
        }
        Ok(())
    }
}

/// Verified identity of one full-resolution scene-linear RAW foundation.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RawFoundationArtifact {
    artifact: GeneratedArtifactReference,
    raster_extent: RasterExtent,
    source: RawFoundationSourceProvenance,
    provenance: RawFoundationProvenance,
}

impl RawFoundationArtifact {
    /// Creates a validated portable output descriptor.
    ///
    /// The artifact content hash is the SHA-256 of the complete
    /// `.shadowrawf` file. The local cache path is intentionally absent.
    ///
    /// # Errors
    ///
    /// Returns an error when the byte identity, encoding, raster extent,
    /// source, or model/cache provenance violates the RAW foundation contract.
    pub fn new(
        artifact: GeneratedArtifactReference,
        raster_extent: RasterExtent,
        source: RawFoundationSourceProvenance,
        provenance: RawFoundationProvenance,
    ) -> Result<Self, RawFoundationArtifactError> {
        let foundation = Self {
            artifact,
            raster_extent,
            source,
            provenance,
        };
        foundation.validate()?;
        Ok(foundation)
    }

    pub const fn artifact(&self) -> &GeneratedArtifactReference {
        &self.artifact
    }

    pub const fn raster_extent(&self) -> RasterExtent {
        self.raster_extent
    }

    pub const fn source(&self) -> &RawFoundationSourceProvenance {
        &self.source
    }

    pub const fn provenance(&self) -> &RawFoundationProvenance {
        &self.provenance
    }

    pub const fn input_domain(&self) -> ImageDomain {
        ImageDomain::SensorMosaic
    }

    pub const fn output_domain(&self) -> ImageDomain {
        ImageDomain::SceneLinearRgb
    }

    pub const fn pixel_layout(&self) -> RasterPixelLayout {
        RasterPixelLayout::RgbPlanar
    }

    pub const fn sample_format(&self) -> RasterSampleFormat {
        RasterSampleFormat::Float32
    }

    pub const fn is_full_resolution(&self) -> bool {
        true
    }

    /// Validates the complete portable RAW foundation descriptor.
    ///
    /// # Errors
    ///
    /// Returns an error when its byte identity, encoding, extent, source, or
    /// model/cache provenance is invalid.
    pub fn validate(&self) -> Result<(), RawFoundationArtifactError> {
        self.artifact.validate()?;
        if self.artifact.hash_algorithm() != ArtifactHashAlgorithm::Sha256 {
            return Err(RawFoundationArtifactError::RequiresSha256);
        }
        validate_sha256(
            self.artifact.content_hash(),
            RawFoundationDigestField::ArtifactFile,
        )?;
        if self.artifact.media_type() != RAW_FOUNDATION_MEDIA_TYPE {
            return Err(RawFoundationArtifactError::UnexpectedMediaType);
        }
        if self.artifact.encoding_version() != RAW_FOUNDATION_ENCODING_VERSION {
            return Err(RawFoundationArtifactError::UnsupportedEncodingVersion(
                self.artifact.encoding_version(),
            ));
        }
        self.raster_extent.validate()?;
        self.source.validate()?;
        self.provenance.validate()
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RawFoundationDigestField {
    ArtifactFile,
    SourceFile,
    SourcePixelContract,
    CacheKey,
    ArtifactIdentity,
    ModelPackage,
    ModelGraph,
}

#[derive(Debug, Clone, Eq, PartialEq, Error)]
pub enum RawFoundationArtifactError {
    #[error(transparent)]
    InvalidGeneratedArtifact(#[from] AiArtifactContractError),
    #[error("RAW foundation generated bytes must use SHA-256 identity")]
    RequiresSha256,
    #[error("RAW foundation artifact uses an unexpected media type")]
    UnexpectedMediaType,
    #[error("RAW foundation encoding version {0} is unsupported")]
    UnsupportedEncodingVersion(u32),
    #[error("RAW foundation {0:?} digest must be canonical lowercase SHA-256")]
    InvalidDigest(RawFoundationDigestField),
    #[error("RAW foundation source file must not be empty")]
    EmptySourceFile,
    #[error("RAW foundation implementation revision is empty or too long")]
    InvalidImplementationRevision,
}

fn validate_sha256(
    digest: &str,
    field: RawFoundationDigestField,
) -> Result<(), RawFoundationArtifactError> {
    if digest.len() != 64
        || !digest
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(RawFoundationArtifactError::InvalidDigest(field));
    }
    Ok(())
}

#[cfg(test)]
mod tests;

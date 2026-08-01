//! Bounded, path-free transfer of one fully verified AI RAW foundation into C++.
//!
//! Artifact parsing, source hashing, model-package admission, and cache verification remain the
//! caller's responsibility. This module validates the complete memory/identity shape and owns the
//! interleaved float buffer while C++ borrows it synchronously for preview or detail preparation.

use shadow_domain::ImageDimensions;

use super::{BridgeError, ffi};

/// Exact public model admitted by the native foundation source route.
pub const RAW_FOUNDATION_MODEL_IDENTITY: &str = "rawnind-public-bayer-release-5.6.0";
/// Pixel-contract implementation revision paired with [`RAW_FOUNDATION_MODEL_IDENTITY`].
pub const RAW_FOUNDATION_IMPLEMENTATION_REVISION: &str = "rawnind-public-bayer-foundation-v1";
/// A transfer can never exceed the native materialized scene-linear retained-buffer limit.
pub const MAX_RAW_FOUNDATION_TRANSFER_BYTES: usize = 1_024 * 1_024 * 1_024;

/// Path-free digests returned by the verified artifact/cache owner.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RawFoundationArtifactIdentity {
    source_digest: String,
    artifact_fingerprint: String,
    cache_key: String,
}

impl RawFoundationArtifactIdentity {
    /// Constructs the digest tuple after the caller has verified the source and artifact bytes.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidRawFoundation`] unless every digest is exactly 64 lowercase
    /// hexadecimal characters.
    pub fn from_verified_digests(
        source_sha256: impl Into<String>,
        artifact_file_sha256: impl Into<String>,
        cache_key_sha256: impl Into<String>,
    ) -> Result<Self, BridgeError> {
        let identity = Self {
            source_digest: source_sha256.into(),
            artifact_fingerprint: artifact_file_sha256.into(),
            cache_key: cache_key_sha256.into(),
        };
        if !canonical_sha256(&identity.source_digest)
            || !canonical_sha256(&identity.artifact_fingerprint)
            || !canonical_sha256(&identity.cache_key)
        {
            return Err(BridgeError::InvalidRawFoundation(
                "source, artifact, and cache-key digests must be lowercase SHA-256",
            ));
        }
        Ok(identity)
    }

    #[must_use]
    pub fn source_sha256(&self) -> &str {
        &self.source_digest
    }

    #[must_use]
    pub fn artifact_file_sha256(&self) -> &str {
        &self.artifact_fingerprint
    }

    #[must_use]
    pub fn cache_key_sha256(&self) -> &str {
        &self.cache_key
    }
}

/// One verified, interleaved linear-camera-RGB foundation ready for synchronous preparation.
///
/// This value owns the large pixel buffer. CXX passes it by const reference; C++ creates a
/// `std::span<const float>` for the duration of the call and the returned preview/detail session
/// owns an independent scene-linear result.
#[derive(Debug)]
pub struct VerifiedRawFoundation {
    ffi: ffi::FfiRawFoundation,
}

impl VerifiedRawFoundation {
    /// Seals verified artifact parts into the sole bridge transfer contract.
    ///
    /// `crop_top` and `crop_left` are the zero-or-one canonical-RGGB crop selected by public
    /// `RawNIND` preprocessing. `samples` must contain exactly `width * height * 3` finite values.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidRawFoundation`] for invalid dimensions/crop, overflow,
    /// excessive memory, sample-count mismatch, or non-finite pixels.
    pub fn from_verified_interleaved_camera_rgb(
        dimensions: ImageDimensions,
        crop_top: u32,
        crop_left: u32,
        amount_percent: u8,
        identity: RawFoundationArtifactIdentity,
        samples: Vec<f32>,
    ) -> Result<Self, BridgeError> {
        if dimensions.width == 0 || dimensions.height == 0 {
            return Err(BridgeError::InvalidRawFoundation(
                "dimensions must be non-zero",
            ));
        }
        if crop_top > 1 || crop_left > 1 {
            return Err(BridgeError::InvalidRawFoundation(
                "canonical Bayer crop offsets must be zero or one",
            ));
        }
        if amount_percent > 100 {
            return Err(BridgeError::InvalidRawFoundation(
                "amount must be between 0 and 100 percent",
            ));
        }
        let sample_count = usize::try_from(dimensions.width)
            .ok()
            .and_then(|width| {
                usize::try_from(dimensions.height)
                    .ok()
                    .and_then(|height| width.checked_mul(height))
            })
            .and_then(|pixels| pixels.checked_mul(3))
            .ok_or(BridgeError::InvalidRawFoundation(
                "sample count exceeds the platform address space",
            ))?;
        let byte_count =
            sample_count
                .checked_mul(size_of::<f32>())
                .ok_or(BridgeError::InvalidRawFoundation(
                    "sample byte count exceeds the platform address space",
                ))?;
        if byte_count > MAX_RAW_FOUNDATION_TRANSFER_BYTES {
            return Err(BridgeError::InvalidRawFoundation(
                "sample buffer exceeds the 1 GiB transfer limit",
            ));
        }
        if samples.len() != sample_count {
            return Err(BridgeError::InvalidRawFoundation(
                "sample count must equal width * height * 3",
            ));
        }
        if samples.iter().any(|value| !value.is_finite()) {
            return Err(BridgeError::InvalidRawFoundation(
                "all camera-RGB samples must be finite",
            ));
        }
        Ok(Self {
            ffi: ffi::FfiRawFoundation {
                width: dimensions.width,
                height: dimensions.height,
                crop_top,
                crop_left,
                amount_percent,
                source_sha256: identity.source_digest,
                artifact_file_sha256: identity.artifact_fingerprint,
                cache_key_sha256: identity.cache_key,
                model_identity: RAW_FOUNDATION_MODEL_IDENTITY.to_owned(),
                implementation_revision: RAW_FOUNDATION_IMPLEMENTATION_REVISION.to_owned(),
                samples,
            },
        })
    }

    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        ImageDimensions {
            width: self.ffi.width,
            height: self.ffi.height,
        }
    }

    #[must_use]
    pub const fn crop_top(&self) -> u32 {
        self.ffi.crop_top
    }

    #[must_use]
    pub const fn crop_left(&self) -> u32 {
        self.ffi.crop_left
    }

    #[must_use]
    pub const fn amount_percent(&self) -> u8 {
        self.ffi.amount_percent
    }

    #[must_use]
    pub fn artifact_identity(&self) -> RawFoundationArtifactIdentity {
        RawFoundationArtifactIdentity {
            source_digest: self.ffi.source_sha256.clone(),
            artifact_fingerprint: self.ffi.artifact_file_sha256.clone(),
            cache_key: self.ffi.cache_key_sha256.clone(),
        }
    }

    pub(crate) const fn ffi(&self) -> &ffi::FfiRawFoundation {
        &self.ffi
    }
}

fn canonical_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
mod tests;

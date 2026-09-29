use serde::{Deserialize, Serialize};
use shadow_domain::{ImageDomain, MaskCoordinateSpace};
use thiserror::Error;

use crate::{AiTaskKind, UnitInterval};

pub const AI_GENERATED_ARTIFACT_CONTRACT_VERSION: u32 = 1;
pub const MAX_MASK_PROMPT_POINTS: usize = 64;
pub const MAX_RASTER_DIMENSION: u32 = 262_144;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ArtifactHashAlgorithm {
    Blake3_256,
    Sha256,
}

/// One content-addressed output produced by an AI worker.
///
/// This is a byte identity, not durable-storage authority. A worker may create
/// it for a rebuildable proposal; only the managed-store transaction can wrap
/// it in a [`crate::ManagedGeneratedArtifactReference`].
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct GeneratedArtifactReference {
    contract_version: u32,
    hash_algorithm: ArtifactHashAlgorithm,
    content_hash: String,
    byte_len: u64,
    media_type: String,
    encoding_version: u32,
}

impl GeneratedArtifactReference {
    /// Creates one validated rebuildable generated-byte identity.
    ///
    /// # Errors
    ///
    /// Returns an error for a malformed hash, empty payload, media type, or
    /// encoding revision.
    pub fn new(
        hash_algorithm: ArtifactHashAlgorithm,
        content_hash: String,
        byte_len: u64,
        media_type: String,
        encoding_version: u32,
    ) -> Result<Self, AiArtifactContractError> {
        let artifact = Self {
            contract_version: AI_GENERATED_ARTIFACT_CONTRACT_VERSION,
            hash_algorithm,
            content_hash,
            byte_len,
            media_type,
            encoding_version,
        };
        artifact.validate()?;
        Ok(artifact)
    }

    pub const fn contract_version(&self) -> u32 {
        self.contract_version
    }

    pub const fn hash_algorithm(&self) -> ArtifactHashAlgorithm {
        self.hash_algorithm
    }

    pub fn content_hash(&self) -> &str {
        &self.content_hash
    }

    pub const fn byte_len(&self) -> u64 {
        self.byte_len
    }

    pub fn media_type(&self) -> &str {
        &self.media_type
    }

    pub const fn encoding_version(&self) -> u32 {
        self.encoding_version
    }

    /// Validates the portable identity and encoding contract.
    ///
    /// # Errors
    ///
    /// Returns an error for unsupported contract versions, non-canonical
    /// digests, empty payloads, or missing encoding metadata.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        if self.contract_version != AI_GENERATED_ARTIFACT_CONTRACT_VERSION {
            return Err(AiArtifactContractError::UnsupportedArtifactContract(
                self.contract_version,
            ));
        }
        if self.content_hash.len() != 64
            || !self
                .content_hash
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit())
        {
            return Err(AiArtifactContractError::InvalidContentHash);
        }
        if self.byte_len == 0 {
            return Err(AiArtifactContractError::EmptyArtifact);
        }
        if self.media_type.trim().is_empty() || self.media_type.len() > 256 {
            return Err(AiArtifactContractError::InvalidMediaType);
        }
        if self.encoding_version == 0 {
            return Err(AiArtifactContractError::InvalidEncodingVersion);
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RasterExtent {
    pub width: u32,
    pub height: u32,
}

impl RasterExtent {
    /// Creates a bounded raster extent.
    ///
    /// # Errors
    ///
    /// Returns an error for zero or implausibly large dimensions.
    pub fn new(width: u32, height: u32) -> Result<Self, AiArtifactContractError> {
        let extent = Self { width, height };
        extent.validate()?;
        Ok(extent)
    }

    /// Validates non-zero dimensions against the portable raster bound.
    ///
    /// # Errors
    ///
    /// Returns an error for zero or implausibly large dimensions.
    pub fn validate(self) -> Result<(), AiArtifactContractError> {
        if self.width == 0 || self.height == 0 {
            return Err(AiArtifactContractError::EmptyRasterExtent);
        }
        if self.width > MAX_RASTER_DIMENSION || self.height > MAX_RASTER_DIMENSION {
            return Err(AiArtifactContractError::RasterExtentTooLarge {
                width: self.width,
                height: self.height,
            });
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MaskPointPolarity {
    Foreground,
    Background,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MaskPromptPoint {
    pub x: UnitInterval,
    pub y: UnitInterval,
    pub polarity: MaskPointPolarity,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NormalizedMaskBox {
    pub left: UnitInterval,
    pub top: UnitInterval,
    pub right: UnitInterval,
    pub bottom: UnitInterval,
}

impl NormalizedMaskBox {
    /// Validates that the normalized box has positive extent on both axes.
    ///
    /// # Errors
    ///
    /// Returns an error when either pair of bounds is reversed or equal.
    pub fn validate(self) -> Result<(), AiArtifactContractError> {
        if self.left >= self.right || self.top >= self.bottom {
            return Err(AiArtifactContractError::InvalidMaskBox);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
pub enum MaskPrompt {
    AutomaticSubject,
    Points {
        #[serde(deserialize_with = "crate::wire_v1::vec_64")]
        points: Vec<MaskPromptPoint>,
    },
    Box {
        bounds: NormalizedMaskBox,
    },
}

impl MaskPrompt {
    /// Validates the selected prompt shape and its bounded payload.
    ///
    /// # Errors
    ///
    /// Returns an error for an empty or oversized point prompt, a point prompt
    /// without foreground evidence, or a degenerate box.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        match self {
            Self::AutomaticSubject => Ok(()),
            Self::Points { points } => {
                if points.is_empty() || points.len() > MAX_MASK_PROMPT_POINTS {
                    return Err(AiArtifactContractError::InvalidMaskPointCount(points.len()));
                }
                if !points
                    .iter()
                    .any(|point| point.polarity == MaskPointPolarity::Foreground)
                {
                    return Err(AiArtifactContractError::MaskHasNoForegroundPoint);
                }
                Ok(())
            }
            Self::Box { bounds } => bounds.validate(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SubjectMaskParameters {
    pub prompt: MaskPrompt,
    pub coordinate_space: MaskCoordinateSpace,
    /// Full coordinate extent to which the lower-resolution soft mask maps.
    pub coordinate_extent: RasterExtent,
    pub edge_refinement: UnitInterval,
    pub maximum_candidates: u8,
}

impl SubjectMaskParameters {
    /// Validates the complete subject-mask request contract.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid prompt or coordinate extent, or for a
    /// candidate count outside `1..=4`.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        self.prompt.validate()?;
        self.coordinate_extent.validate()?;
        if !(1..=4).contains(&self.maximum_candidates) {
            return Err(AiArtifactContractError::InvalidMaskCandidateCount(
                self.maximum_candidates,
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DenoiseDomainPolicy {
    SensorMosaicRequired,
    SensorMosaicPreferred,
    SceneLinearRgb,
    RenderedRgbFallback,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DenoiseQuality {
    InteractivePreview,
    Standard,
    Final,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DenoiseParameters {
    pub domain_policy: DenoiseDomainPolicy,
    pub quality: DenoiseQuality,
    pub strength: UnitInterval,
    pub detail_protection: UnitInterval,
    pub chroma_reduction: UnitInterval,
}

/// Provider-neutral input identity for one bounded image-completion proposal.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImageCompletionParameters {
    pub coordinate_extent: RasterExtent,
    pub source_recipe_blake3: String,
    pub mask_revision: String,
}

impl ImageCompletionParameters {
    /// Validates the exact source, mask, and coordinate identity of a proposal.
    ///
    /// # Errors
    ///
    /// Returns an error when the extent is invalid or either digest is not a
    /// canonical 256-bit lowercase hexadecimal identity.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        self.coordinate_extent.validate()?;
        if !canonical_digest(&self.source_recipe_blake3) {
            return Err(AiArtifactContractError::InvalidCompletionSourceIdentity);
        }
        if !canonical_digest(&self.mask_revision) {
            return Err(AiArtifactContractError::InvalidCompletionMaskIdentity);
        }
        Ok(())
    }
}

impl DenoiseParameters {
    /// Validates the denoise parameter contract.
    ///
    /// # Errors
    ///
    /// Reserved for future contract revisions; the current strongly typed
    /// fields are valid by construction.
    pub const fn validate(&self) -> Result<(), AiArtifactContractError> {
        Ok(())
    }
}

/// Typed task parameters carried by the stable worker envelope.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
pub enum AiTaskParameters {
    #[default]
    None,
    SubjectMask(SubjectMaskParameters),
    Denoise(DenoiseParameters),
    ImageCompletion(ImageCompletionParameters),
    /// Provider-neutral request to materialize the photo's full RAW foundation.
    RawFoundation,
}

impl AiTaskParameters {
    /// Ensures a task cannot accidentally be executed with another task's
    /// parameters after crossing a process or persistence boundary.
    ///
    /// # Errors
    ///
    /// Returns an error when the parameter variant does not match `task` or
    /// when the selected parameter payload is invalid.
    pub fn validate_for(&self, task: AiTaskKind) -> Result<(), AiArtifactContractError> {
        match (task, self) {
            (AiTaskKind::ProposeSubjectMask, Self::SubjectMask(parameters)) => {
                parameters.validate()
            }
            (AiTaskKind::Denoise, Self::Denoise(parameters)) => parameters.validate(),
            (AiTaskKind::MaterializeRawFoundation, Self::RawFoundation) => Ok(()),
            (AiTaskKind::GenerateInpaintPatch, Self::ImageCompletion(parameters)) => {
                parameters.validate()
            }
            (AiTaskKind::ProposeSubjectMask, _) => {
                Err(AiArtifactContractError::TaskParameterMismatch {
                    task,
                    expected: "subject_mask",
                })
            }
            (AiTaskKind::Denoise, _) => Err(AiArtifactContractError::TaskParameterMismatch {
                task,
                expected: "denoise",
            }),
            (AiTaskKind::MaterializeRawFoundation, _) => {
                Err(AiArtifactContractError::TaskParameterMismatch {
                    task,
                    expected: "raw_foundation",
                })
            }
            (AiTaskKind::GenerateInpaintPatch, _) => {
                Err(AiArtifactContractError::TaskParameterMismatch {
                    task,
                    expected: "image_completion",
                })
            }
            (_, Self::None) => Ok(()),
            (
                _,
                Self::SubjectMask(_)
                | Self::Denoise(_)
                | Self::ImageCompletion(_)
                | Self::RawFoundation,
            ) => Err(AiArtifactContractError::UnexpectedTaskParameters(task)),
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SoftMaskEncoding {
    Gray8Unorm,
    Gray16Float,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
pub enum MaskSemantic {
    Subject,
    Object,
    Person,
    FaceSkin,
    BodySkin,
    Sky,
    Background,
    LandscapeRegion { label: String },
    UserPrompt,
}

impl MaskSemantic {
    fn validate(&self) -> Result<(), AiArtifactContractError> {
        if let Self::LandscapeRegion { label } = self
            && label.trim().is_empty()
        {
            return Err(AiArtifactContractError::InvalidSemanticLabel);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SoftMaskArtifact {
    pub artifact: GeneratedArtifactReference,
    /// Physical resolution of the stored mask raster.
    pub raster_extent: RasterExtent,
    /// Full image extent represented by the mask.
    pub coordinate_extent: RasterExtent,
    pub coordinate_space: MaskCoordinateSpace,
    pub encoding: SoftMaskEncoding,
    pub semantic: MaskSemantic,
}

impl SoftMaskArtifact {
    /// Validates the referenced mask artifact and both raster extents.
    ///
    /// # Errors
    ///
    /// Returns an error when the artifact identity, extent, or semantic label
    /// violates its portable contract.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        self.artifact.validate()?;
        self.raster_extent.validate()?;
        self.coordinate_extent.validate()?;
        self.semantic.validate()?;
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RasterSampleFormat {
    Uint16,
    Float16,
    Float32,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RasterPixelLayout {
    SensorMosaic,
    RgbPlanar,
    RgbInterleaved,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TileContract {
    pub tile_edge: u32,
    pub halo: u32,
}

impl TileContract {
    /// Validates that the tile interior remains non-empty after both halos.
    ///
    /// # Errors
    ///
    /// Returns an error for a zero edge or a halo at least half the tile edge.
    pub fn validate(self) -> Result<(), AiArtifactContractError> {
        if self.tile_edge == 0 || self.halo.saturating_mul(2) >= self.tile_edge {
            return Err(AiArtifactContractError::InvalidTileContract {
                tile_edge: self.tile_edge,
                halo: self.halo,
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DenoisedRasterArtifact {
    pub artifact: GeneratedArtifactReference,
    pub input_domain: ImageDomain,
    pub output_domain: ImageDomain,
    pub raster_extent: RasterExtent,
    pub pixel_layout: RasterPixelLayout,
    pub sample_format: RasterSampleFormat,
    /// Exact `RawFrame` or decoded-raster contract from which this result arose.
    pub source_pixel_contract_hash: String,
    pub tile_contract: Option<TileContract>,
    pub full_resolution: bool,
}

/// Tightly packed RGBA8 or little-endian linear RGBA32F completion bytes.
/// alpha is the exact user selection, so pixels outside it are immutable.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImageCompletionPatchArtifact {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source_context: Option<shadow_domain::ImageCompletionSourceContext>,
    pub artifact: GeneratedArtifactReference,
    pub raster_extent: RasterExtent,
    pub coordinate_extent: RasterExtent,
    pub source_recipe_blake3: String,
    pub provider: String,
    pub deployment: String,
    pub model_build: String,
    pub postprocessing_identity: String,
    pub api_contract_revision: String,
    pub actual_execution_provider: String,
}

impl ImageCompletionPatchArtifact {
    /// Whether the admitted raster uses little-endian working-linear float samples.
    pub fn linear_rgba_f32(&self) -> bool {
        self.artifact.media_type() == "application/x-shadow-linear-rgba-f32"
    }

    /// Validates the generated raster and its complete execution provenance.
    ///
    /// # Errors
    ///
    /// Returns an error when the artifact identity, dimensions, byte count,
    /// source digest, media encoding, or provenance is outside the admitted
    /// completion-patch contract.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        self.artifact.validate()?;
        if let Some(context) = &self.source_context {
            if !self.linear_rgba_f32() || context.validate().is_err() {
                return Err(AiArtifactContractError::InvalidCompletionSourceIdentity);
            }
        }

        self.raster_extent.validate()?;
        self.coordinate_extent.validate()?;
        let expected = u64::from(self.raster_extent.width)
            .checked_mul(u64::from(self.raster_extent.height))
            .and_then(|pixels| pixels.checked_mul(if self.linear_rgba_f32() { 16 } else { 4 }))
            .ok_or(AiArtifactContractError::CompletionPatchTooLarge)?;
        if self.artifact.byte_len() != expected {
            return Err(AiArtifactContractError::CompletionPatchByteLengthMismatch {
                expected,
                actual: self.artifact.byte_len(),
            });
        }
        if self.artifact.encoding_version() != 1
            || (self.artifact.media_type() != "application/x-shadow-rgba8"
                && !self.linear_rgba_f32())
        {
            return Err(AiArtifactContractError::InvalidCompletionPatchMediaType);
        }
        if !canonical_digest(&self.source_recipe_blake3) {
            return Err(AiArtifactContractError::InvalidCompletionSourceIdentity);
        }
        for value in [
            &self.provider,
            &self.deployment,
            &self.model_build,
            &self.postprocessing_identity,
            &self.api_contract_revision,
            &self.actual_execution_provider,
        ] {
            if value.trim().is_empty() || value.len() > 256 {
                return Err(AiArtifactContractError::InvalidCompletionProvenance);
            }
        }
        Ok(())
    }
}

impl DenoisedRasterArtifact {
    /// Validates artifact identity, domain/layout agreement, and tiling.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid provenance, extents, domain changes,
    /// pixel-layout mismatch, or an invalid tile contract.
    pub fn validate(&self) -> Result<(), AiArtifactContractError> {
        self.artifact.validate()?;
        self.raster_extent.validate()?;
        if self.source_pixel_contract_hash.len() != 64
            || !self
                .source_pixel_contract_hash
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit())
        {
            return Err(AiArtifactContractError::InvalidSourcePixelContractHash);
        }
        if self.input_domain != self.output_domain {
            return Err(AiArtifactContractError::DenoiseChangesImageDomain {
                input: self.input_domain,
                output: self.output_domain,
            });
        }
        match (self.input_domain, self.pixel_layout) {
            (ImageDomain::SensorMosaic, RasterPixelLayout::SensorMosaic)
            | (
                ImageDomain::SceneLinearRgb
                | ImageDomain::WorkingRgb
                | ImageDomain::DisplayReferredRgb,
                RasterPixelLayout::RgbPlanar | RasterPixelLayout::RgbInterleaved,
            ) => {}
            _ => {
                return Err(AiArtifactContractError::PixelLayoutDomainMismatch {
                    domain: self.input_domain,
                    layout: self.pixel_layout,
                });
            }
        }
        if let Some(tile_contract) = self.tile_contract {
            tile_contract.validate()?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
pub enum AiGeneratedPayload {
    SoftMask(SoftMaskArtifact),
    DenoisedRaster(DenoisedRasterArtifact),
    ImageCompletionPatch(ImageCompletionPatchArtifact),
}

impl AiGeneratedPayload {
    /// Validates that this generated payload belongs to `task`.
    ///
    /// # Errors
    ///
    /// Returns an error when the payload variant does not match `task` or its
    /// nested artifact contract is invalid.
    pub fn validate_for(&self, task: AiTaskKind) -> Result<(), AiArtifactContractError> {
        match (task, self) {
            (AiTaskKind::ProposeSubjectMask, Self::SoftMask(mask)) => mask.validate(),
            (AiTaskKind::Denoise, Self::DenoisedRaster(raster)) => raster.validate(),
            (AiTaskKind::GenerateInpaintPatch, Self::ImageCompletionPatch(patch)) => {
                patch.validate()
            }
            (AiTaskKind::ProposeSubjectMask, _) => {
                Err(AiArtifactContractError::TaskPayloadMismatch {
                    task,
                    expected: "soft_mask",
                })
            }
            (AiTaskKind::Denoise, _) => Err(AiArtifactContractError::TaskPayloadMismatch {
                task,
                expected: "denoised_raster",
            }),
            (AiTaskKind::GenerateInpaintPatch, _) => {
                Err(AiArtifactContractError::TaskPayloadMismatch {
                    task,
                    expected: "image_completion_patch",
                })
            }
            _ => Err(AiArtifactContractError::UnexpectedGeneratedPayload(task)),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Error)]
pub enum AiArtifactContractError {
    #[error("unsupported generated-artifact contract version {0}")]
    UnsupportedArtifactContract(u32),
    #[error("generated artifact content hash must be a 256-bit hexadecimal digest")]
    InvalidContentHash,
    #[error("generated artifact is empty")]
    EmptyArtifact,
    #[error("generated artifact media type is empty or too long")]
    InvalidMediaType,
    #[error("generated artifact encoding version must be non-zero")]
    InvalidEncodingVersion,
    #[error("raster dimensions must both be non-zero")]
    EmptyRasterExtent,
    #[error("raster extent {width}x{height} exceeds the supported contract bound")]
    RasterExtentTooLarge { width: u32, height: u32 },
    #[error("mask point count must be between 1 and {MAX_MASK_PROMPT_POINTS}, got {0}")]
    InvalidMaskPointCount(usize),
    #[error("a point-prompt mask requires at least one foreground point")]
    MaskHasNoForegroundPoint,
    #[error("normalized mask box must have positive width and height")]
    InvalidMaskBox,
    #[error("mask candidate count must be between 1 and 4, got {0}")]
    InvalidMaskCandidateCount(u8),
    #[error("task {task:?} requires {expected} parameters")]
    TaskParameterMismatch {
        task: AiTaskKind,
        expected: &'static str,
    },
    #[error("task {0:?} does not accept these generated-artifact parameters")]
    UnexpectedTaskParameters(AiTaskKind),
    #[error("mask semantic label must not be empty")]
    InvalidSemanticLabel,
    #[error("tile edge {tile_edge} must be non-zero and larger than twice halo {halo}")]
    InvalidTileContract { tile_edge: u32, halo: u32 },
    #[error("source pixel-contract hash must be a 256-bit hexadecimal digest")]
    InvalidSourcePixelContractHash,
    #[error("denoise output changed image domain from {input:?} to {output:?}")]
    DenoiseChangesImageDomain {
        input: ImageDomain,
        output: ImageDomain,
    },
    #[error("pixel layout {layout:?} is incompatible with image domain {domain:?}")]
    PixelLayoutDomainMismatch {
        domain: ImageDomain,
        layout: RasterPixelLayout,
    },
    #[error("task {task:?} requires a {expected} payload")]
    TaskPayloadMismatch {
        task: AiTaskKind,
        expected: &'static str,
    },
    #[error("task {0:?} does not produce this generated-artifact payload")]
    UnexpectedGeneratedPayload(AiTaskKind),
    #[error("image-completion source Recipe identity must be a lowercase 256-bit digest")]
    InvalidCompletionSourceIdentity,
    #[error("image-completion mask identity must be a lowercase 256-bit digest")]
    InvalidCompletionMaskIdentity,
    #[error("image-completion patch dimensions overflow the byte contract")]
    CompletionPatchTooLarge,
    #[error("image-completion patch byte length mismatch: expected {expected}, got {actual}")]
    CompletionPatchByteLengthMismatch { expected: u64, actual: u64 },
    #[error("image-completion patch must use a supported Shadow RGBA media type")]
    InvalidCompletionPatchMediaType,
    #[error("image-completion provenance is incomplete or exceeds its bound")]
    InvalidCompletionProvenance,
}

fn canonical_digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
mod tests;

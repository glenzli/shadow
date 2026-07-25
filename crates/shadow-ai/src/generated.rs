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

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum GeneratedArtifactStorageClass {
    /// A result that may be evicted and recomputed from its exact provenance.
    RebuildableProposal,
    /// An accepted edit dependency that must not be evicted like a preview.
    ManagedDerived,
}

/// One content-addressed output produced by an AI worker.
///
/// The worker may only create `RebuildableProposal` artifacts. The application
/// promotes an accepted result into `ManagedDerived` storage before a recipe is
/// allowed to depend on it.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct GeneratedArtifactReference {
    pub contract_version: u32,
    pub hash_algorithm: ArtifactHashAlgorithm,
    pub content_hash: String,
    pub byte_len: u64,
    pub media_type: String,
    pub encoding_version: u32,
    pub storage_class: GeneratedArtifactStorageClass,
}

impl GeneratedArtifactReference {
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
pub struct MaskPromptPoint {
    pub x: UnitInterval,
    pub y: UnitInterval,
    pub polarity: MaskPointPolarity,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct NormalizedMaskBox {
    pub left: UnitInterval,
    pub top: UnitInterval,
    pub right: UnitInterval,
    pub bottom: UnitInterval,
}

impl NormalizedMaskBox {
    pub fn validate(self) -> Result<(), AiArtifactContractError> {
        if self.left >= self.right || self.top >= self.bottom {
            return Err(AiArtifactContractError::InvalidMaskBox);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum MaskPrompt {
    AutomaticSubject,
    Points { points: Vec<MaskPromptPoint> },
    Box { bounds: NormalizedMaskBox },
}

impl MaskPrompt {
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
pub struct SubjectMaskParameters {
    pub prompt: MaskPrompt,
    pub coordinate_space: MaskCoordinateSpace,
    /// Full coordinate extent to which the lower-resolution soft mask maps.
    pub coordinate_extent: RasterExtent,
    pub edge_refinement: UnitInterval,
    pub maximum_candidates: u8,
}

impl SubjectMaskParameters {
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
pub struct DenoiseParameters {
    pub domain_policy: DenoiseDomainPolicy,
    pub quality: DenoiseQuality,
    pub strength: UnitInterval,
    pub detail_protection: UnitInterval,
    pub chroma_reduction: UnitInterval,
}

impl DenoiseParameters {
    pub const fn validate(&self) -> Result<(), AiArtifactContractError> {
        Ok(())
    }
}

/// Typed task parameters carried by the stable worker envelope.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum AiTaskParameters {
    #[default]
    None,
    SubjectMask(SubjectMaskParameters),
    Denoise(DenoiseParameters),
}

impl AiTaskParameters {
    /// Ensures a task cannot accidentally be executed with another task's
    /// parameters after crossing a process or persistence boundary.
    pub fn validate_for(&self, task: AiTaskKind) -> Result<(), AiArtifactContractError> {
        match (task, self) {
            (AiTaskKind::ProposeSubjectMask, Self::SubjectMask(parameters)) => {
                parameters.validate()
            }
            (AiTaskKind::Denoise, Self::Denoise(parameters)) => parameters.validate(),
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
            (_, Self::None) => Ok(()),
            (_, Self::SubjectMask(_)) | (_, Self::Denoise(_)) => {
                Err(AiArtifactContractError::UnexpectedTaskParameters(task))
            }
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
#[serde(rename_all = "snake_case", tag = "kind")]
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
pub struct TileContract {
    pub tile_edge: u32,
    pub halo: u32,
}

impl TileContract {
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
pub struct DenoisedRasterArtifact {
    pub artifact: GeneratedArtifactReference,
    pub input_domain: ImageDomain,
    pub output_domain: ImageDomain,
    pub raster_extent: RasterExtent,
    pub pixel_layout: RasterPixelLayout,
    pub sample_format: RasterSampleFormat,
    /// Exact RawFrame or decoded-raster contract from which this result arose.
    pub source_pixel_contract_hash: String,
    pub tile_contract: Option<TileContract>,
    pub full_resolution: bool,
}

impl DenoisedRasterArtifact {
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
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum AiGeneratedPayload {
    SoftMask(SoftMaskArtifact),
    DenoisedRaster(DenoisedRasterArtifact),
}

impl AiGeneratedPayload {
    pub fn validate_for(&self, task: AiTaskKind) -> Result<(), AiArtifactContractError> {
        match (task, self) {
            (AiTaskKind::ProposeSubjectMask, Self::SoftMask(mask)) => mask.validate(),
            (AiTaskKind::Denoise, Self::DenoisedRaster(raster)) => raster.validate(),
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
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unit(value: f64) -> UnitInterval {
        UnitInterval::new(value).expect("valid unit value")
    }

    fn proposal_artifact() -> GeneratedArtifactReference {
        GeneratedArtifactReference {
            contract_version: AI_GENERATED_ARTIFACT_CONTRACT_VERSION,
            hash_algorithm: ArtifactHashAlgorithm::Blake3_256,
            content_hash: "a".repeat(64),
            byte_len: 1024,
            media_type: "application/x-shadow-soft-mask".to_owned(),
            encoding_version: 1,
            storage_class: GeneratedArtifactStorageClass::RebuildableProposal,
        }
    }

    #[test]
    fn point_mask_requires_a_foreground_prompt() {
        let prompt = MaskPrompt::Points {
            points: vec![MaskPromptPoint {
                x: unit(0.5),
                y: unit(0.5),
                polarity: MaskPointPolarity::Background,
            }],
        };

        assert_eq!(
            prompt.validate(),
            Err(AiArtifactContractError::MaskHasNoForegroundPoint)
        );
    }

    #[test]
    fn generated_payload_must_match_the_job_kind() {
        let payload = AiGeneratedPayload::SoftMask(SoftMaskArtifact {
            artifact: proposal_artifact(),
            raster_extent: RasterExtent::new(256, 256).expect("valid raster"),
            coordinate_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
            coordinate_space: MaskCoordinateSpace::Original,
            encoding: SoftMaskEncoding::Gray8Unorm,
            semantic: MaskSemantic::Subject,
        });

        assert!(payload.validate_for(AiTaskKind::ProposeSubjectMask).is_ok());
        assert!(matches!(
            payload.validate_for(AiTaskKind::Denoise),
            Err(AiArtifactContractError::TaskPayloadMismatch { .. })
        ));
    }

    #[test]
    fn denoise_never_silently_changes_the_pixel_domain() {
        let output = DenoisedRasterArtifact {
            artifact: proposal_artifact(),
            input_domain: ImageDomain::SensorMosaic,
            output_domain: ImageDomain::SceneLinearRgb,
            raster_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
            pixel_layout: RasterPixelLayout::SensorMosaic,
            sample_format: RasterSampleFormat::Float16,
            source_pixel_contract_hash: "b".repeat(64),
            tile_contract: Some(TileContract {
                tile_edge: 512,
                halo: 32,
            }),
            full_resolution: true,
        };

        assert!(matches!(
            output.validate(),
            Err(AiArtifactContractError::DenoiseChangesImageDomain { .. })
        ));
    }

    #[test]
    fn task_parameters_reject_cross_task_reuse() {
        let parameters = AiTaskParameters::SubjectMask(SubjectMaskParameters {
            prompt: MaskPrompt::AutomaticSubject,
            coordinate_space: MaskCoordinateSpace::Original,
            coordinate_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
            edge_refinement: unit(0.5),
            maximum_candidates: 3,
        });

        assert!(
            parameters
                .validate_for(AiTaskKind::ProposeSubjectMask)
                .is_ok()
        );
        assert!(matches!(
            parameters.validate_for(AiTaskKind::Denoise),
            Err(AiArtifactContractError::TaskParameterMismatch { .. })
        ));
    }
}
